// typer.cpp —— 模拟打字工作线程。
//
// 工作线程等待启动事件；被唤醒后取出文本快照，先合成释放用户物理按住的
// Ctrl/Shift（否则注入的字符会被目标程序解释成 Ctrl+C 之类的快捷键），
// 再按 keysim 展开的动作序列逐键注入，每键之间停顿设定的毫秒数。
// 模拟期间每键注入前检查 Esc，一旦按下立即中止剩余内容。
#include <windows.h>
#include <mutex>
#include <utility>
#include "keysim.h"
#include "typer.h"

std::atomic<unsigned> g_speedMs{10};

namespace {

std::mutex g_textMutex;
std::wstring g_pendingText;
HANDLE g_hStartEvent = nullptr;
HANDLE g_hThread = nullptr;
std::atomic<bool> g_active{false};
std::atomic<bool> g_run{false};
std::atomic<bool> g_abortRequested{false};

bool EscPressed() {
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

// 是否请求停止（Esc 物理按下，或钩子在模拟期间置位的中止标志）。
// 消费中止标志：返回 true 后由调用方执行清理。
bool StopRequested() {
    if (EscPressed()) return true;
    return g_abortRequested.exchange(false, std::memory_order_relaxed);
}

// 可中断睡眠：把长睡切成小片，保证 Esc/退出请求的响应延迟有界
// （否则极大速度下单次 Sleep 可达数小时甚至被 Sleep 视作 INFINITE）。
// 返回 false 表示睡眠期间收到中止/退出请求。
bool InterruptibleSleep(unsigned ms) {
    while (ms > 0) {
        const unsigned slice = ms < 50 ? ms : 50;
        Sleep(slice);
        ms -= slice;
        if (StopRequested() || !g_run.load(std::memory_order_relaxed)) {
            return false;
        }
    }
    return true;
}

// 注入 1~2 个 UTF-16 码元（先全部按下，再全部抬起）。
void PressUnicode(const wchar_t* units, int count) {
    INPUT in[4]{};
    for (int k = 0; k < count; ++k) {
        in[k].type = INPUT_KEYBOARD;
        in[k].ki.wScan = static_cast<WORD>(units[k]);
        in[k].ki.dwFlags = KEYEVENTF_UNICODE;
    }
    for (int k = 0; k < count; ++k) {
        in[count + k].type = INPUT_KEYBOARD;
        in[count + k].ki.wScan = static_cast<WORD>(units[k]);
        in[count + k].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    SendInput(static_cast<UINT>(count * 2), in, sizeof(INPUT));
}

// 释放用户物理按住的修饰键，避免注入内容被解释成快捷键。
// 同时抬起通用键与 L/R 具体键：不同程序读取不同槽位，未按下的键注入 up 无害。
// 返回 false 表示注入被系统阻断（UIPI 提权窗口、安全桌面等），调用方应放弃本次模拟。
bool ReleaseModifiers() {
    INPUT in[6]{};
    const WORD keys[6] = {VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
                          VK_SHIFT, VK_LSHIFT, VK_RSHIFT};
    for (int k = 0; k < 6; ++k) {
        in[k].type = INPUT_KEYBOARD;
        in[k].ki.wVk = keys[k];
        in[k].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    return SendInput(6, in, sizeof(INPUT)) == 6;
}

void ExecuteAction(const keysim::Action& a) {
    switch (a.kind) {
    case keysim::ActionKind::TapVk: {
        // 单次 SendInput 原子生效：进程若在动作间被终止，不会残留按下态的键。
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = a.vk;
        in[0].ki.dwFlags = a.extended ? KEYEVENTF_EXTENDEDKEY : 0;
        in[1] = in[0];
        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        break;
    }
    case keysim::ActionKind::Unicode:
        PressUnicode(a.units, a.unitCount);
        break;
    case keysim::ActionKind::CtrlBackspace: {
        // 单次 SendInput 原子生效：进程若在动作间被终止，不会残留半开的 Ctrl。
        INPUT in[4]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = VK_BACK;
        in[2].type = INPUT_KEYBOARD;
        in[2].ki.wVk = VK_BACK;
        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD;
        in[3].ki.wVk = VK_CONTROL;
        in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
        break;
    }
    }
}

DWORD WINAPI TyperThreadProc(LPVOID) {
    while (g_run.load(std::memory_order_relaxed)) {
        if (WaitForSingleObject(g_hStartEvent, INFINITE) != WAIT_OBJECT_0) break;
        if (!g_run.load(std::memory_order_relaxed)) break;

        std::wstring text;
        {
            std::lock_guard<std::mutex> lock(g_textMutex);
            text.swap(g_pendingText);
        }

        if (!ReleaseModifiers()) {
            // 无法清除修饰键状态：继续注入会被目标程序解释成快捷键，放弃本次模拟。
            g_active.store(false, std::memory_order_release);
            continue;
        }
        Sleep(15); // 等待修饰键释放状态在目标程序中生效

        try {
            const std::vector<keysim::Action> actions = keysim::ExpandText(text);
            for (const keysim::Action& a : actions) {
                if (StopRequested()) break;                        // 用户要求中止
                if (!g_run.load(std::memory_order_relaxed)) break; // 软件正在退出
                ExecuteAction(a);
                if (!InterruptibleSleep(g_speedMs.load(std::memory_order_relaxed))) {
                    break;
                }
            }
        } catch (...) {
            // 内存不足等异常：放弃本次模拟，避免异常穿出线程过程触发
            // std::terminate 静默杀死整个常驻进程。
        }

        ReleaseModifiers(); // 收尾/中止时确保不残留逻辑按下的修饰键（尽力而为）
        g_active.store(false, std::memory_order_release);
    }
    return 0;
}

} // namespace

bool TypingActive() {
    return g_active.load(std::memory_order_acquire);
}

void RequestTypingAbort() {
    g_abortRequested.store(true, std::memory_order_relaxed);
}

bool StartTypingFromText(std::wstring text) {
    if (g_active.exchange(true)) {
        return false; // 正在模拟中：忽略本次请求
    }
    // 在请求线程上复位过期的中止标志（本函数与 RequestTypingAbort 同在钩子
    // 线程，天然串行）：此后钩子置位的标志只会被 worker 消费，不会在模拟
    // 启动窗口内被误清，导致“Esc 按了却被无视”。
    g_abortRequested.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_textMutex);
        g_pendingText = std::move(text);
    }
    SetEvent(g_hStartEvent);
    return true;
}

bool InitTyper() {
    g_hStartEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_hStartEvent) return false;
    g_run.store(true, std::memory_order_relaxed);
    g_hThread = CreateThread(nullptr, 0, TyperThreadProc, nullptr, 0, nullptr);
    if (!g_hThread) {
        g_run.store(false, std::memory_order_relaxed);
        CloseHandle(g_hStartEvent);
        g_hStartEvent = nullptr;
        return false;
    }
    return true;
}

void ShutdownTyper() {
    g_run.store(false, std::memory_order_relaxed);
    if (g_hStartEvent) SetEvent(g_hStartEvent);
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 500);
        CloseHandle(g_hThread);
        g_hThread = nullptr;
    }
    if (g_hStartEvent) {
        CloseHandle(g_hStartEvent);
        g_hStartEvent = nullptr;
    }
}
