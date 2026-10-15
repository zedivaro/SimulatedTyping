// typer.cpp —— 模拟打字工作线程。
//
// 工作线程等待启动事件；被唤醒后取出文本快照，先合成释放用户物理按住的
// Ctrl/Shift（否则注入的字符会被目标程序解释成 Ctrl+C 之类的快捷键），
// 再按 keysim 展开的动作组逐键注入：先打占位字符 X 并把光标移到其左侧，
// 内容字符依次插入在 X 之前，每键之间停顿设定的毫秒数。
// 模拟期间每键注入前检查中止请求（Esc / 退出），一旦触发立即停止剩余
// 内容，补注当前组的中止清理动作（撤销未就位的占位 X / 补删换行后的
// Space），最后注入 Delete 删除占位 X（Leader 未完成时无需 Delete）。
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

// 注入一个动作。返回 false 表示注入被系统丢弃（如输入队列满）：调用方
// 应立即中止本次模拟——「占位 X 恒在光标右侧」的不变量已不可信。
bool ExecuteAction(const keysim::Action& a) {
    switch (a.kind) {
    case keysim::ActionKind::TapVk: {
        // 单次 SendInput 原子生效：进程若在动作间被终止，不会残留按下态的键。
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = a.vk;
        in[0].ki.dwFlags = a.extended ? KEYEVENTF_EXTENDEDKEY : 0;
        in[1] = in[0];
        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        return SendInput(2, in, sizeof(INPUT)) == 2;
    }
    case keysim::ActionKind::Unicode: {
        // 注入 1~2 个 UTF-16 码元（先全部按下，再全部抬起）。
        INPUT in[4]{};
        for (int k = 0; k < a.unitCount; ++k) {
            in[k].type = INPUT_KEYBOARD;
            in[k].ki.wScan = static_cast<WORD>(a.units[k]);
            in[k].ki.dwFlags = KEYEVENTF_UNICODE;
        }
        for (int k = 0; k < a.unitCount; ++k) {
            in[a.unitCount + k].type = INPUT_KEYBOARD;
            in[a.unitCount + k].ki.wScan = static_cast<WORD>(a.units[k]);
            in[a.unitCount + k].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        }
        return SendInput(static_cast<UINT>(a.unitCount * 2), in, sizeof(INPUT)) ==
               static_cast<UINT>(a.unitCount * 2);
    }
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
        return SendInput(4, in, sizeof(INPUT)) == 4;
    }
    }
    return true;
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

        bool leaderDone = false;
        bool deleteSent = false;
        try {
            const std::vector<keysim::ActionGroup> groups = keysim::ExpandTextGroups(text);
            bool aborted = false;
            for (const keysim::ActionGroup& group : groups) {
                size_t completed = 0;
                for (const keysim::Action& a : group.actions) {
                    if (StopRequested()) {
                        aborted = true; // 用户要求中止
                        break;
                    }
                    if (!g_run.load(std::memory_order_relaxed)) {
                        aborted = true; // 软件正在退出
                        break;
                    }
                    if (!ExecuteAction(a)) {
                        // 注入失败：占位 X 恒在光标右侧的不变量已不可信，
                        // 立即中止且不计入已完成数，避免结尾 Delete 误删文档内容。
                        aborted = true;
                        break;
                    }
                    ++completed;
                    if (!InterruptibleSleep(g_speedMs.load(std::memory_order_relaxed))) {
                        aborted = true;
                        break;
                    }
                }
                if (group.kind == keysim::ActionGroupKind::Leader &&
                    completed == group.actions.size()) {
                    leaderDone = true; // 占位 X 已就位：结束/中止时需 Delete 删除
                }
                if (aborted) {
                    // 中止清理：立即（不按速度停顿）补注清除残留辅助键的动作
                    // （撤销未就位的占位 X / 补删换行后的 Space），期间不再
                    // 响应中止，保证清理完整。清理动作本身注入失败按尽力
                    // 而为处理（ExecuteAction 不抛异常）。
                    for (const keysim::Action& a :
                         keysim::AbortCleanup(group, completed)) {
                        ExecuteAction(a);
                    }
                    break;
                }
            }
            if (leaderDone) {
                // 模拟结束或中止：删除光标右侧的占位字符 X。
                ExecuteAction(keysim::PlaceholderDeleteAction());
                deleteSent = true;
            }
        } catch (...) {
            // 内存不足等异常：放弃本次模拟，避免异常穿出线程过程触发
            // std::terminate 静默杀死整个常驻进程。
        }
        if (leaderDone && !deleteSent) {
            // 异常路径收尾：异常可能发生在展开或清理阶段，此时占位 X 仍在
            // 光标右侧，尽力补发结尾 Delete（ExecuteAction 不抛异常）。
            ExecuteAction(keysim::PlaceholderDeleteAction());
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
        // worker 离开注入循环后 ≤50ms 必退，但大文本的 ExpandText 分配可达
        // 秒级——给足等待，避免 ExitProcess 恰逢 worker 持堆锁时孤儿化堆
        // 临界区导致退出挂死。
        for (int i = 0; i < 4; ++i) {
            if (WaitForSingleObject(g_hThread, 500) == WAIT_OBJECT_0) break;
        }
        CloseHandle(g_hThread);
        g_hThread = nullptr;
    }
    if (g_hStartEvent) {
        CloseHandle(g_hStartEvent);
        g_hStartEvent = nullptr;
    }
}
