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

bool EscPressed() {
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

void PressVk(WORD vk, bool up, bool extended) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
    if (up) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
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
void ReleaseModifiers() {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[0].ki.dwFlags = KEYEVENTF_KEYUP;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_SHIFT;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void ExecuteAction(const keysim::Action& a) {
    switch (a.kind) {
    case keysim::ActionKind::TapVk:
        PressVk(a.vk, false, a.extended);
        PressVk(a.vk, true, a.extended);
        break;
    case keysim::ActionKind::Unicode:
        PressUnicode(a.units, a.unitCount);
        break;
    case keysim::ActionKind::CtrlBackspace:
        PressVk(VK_CONTROL, false, false);
        PressVk(VK_BACK, false, false);
        PressVk(VK_BACK, true, false);
        PressVk(VK_CONTROL, true, false);
        break;
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

        g_active.store(true, std::memory_order_release);
        ReleaseModifiers();
        Sleep(15); // 等待修饰键释放状态在目标程序中生效

        const std::vector<keysim::Action> actions = keysim::ExpandText(text);
        for (const keysim::Action& a : actions) {
            if (EscPressed()) break; // 用户要求中止
            ExecuteAction(a);
            Sleep(g_speedMs.load(std::memory_order_relaxed));
        }

        ReleaseModifiers(); // 中止时确保不残留逻辑按下的修饰键
        g_active.store(false, std::memory_order_release);
    }
    return 0;
}

} // namespace

bool TypingActive() {
    return g_active.load(std::memory_order_acquire);
}

bool StartTypingFromText(std::wstring text) {
    if (g_active.exchange(true)) {
        return false; // 正在模拟中：忽略本次请求
    }
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
