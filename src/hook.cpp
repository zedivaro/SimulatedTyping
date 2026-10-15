// hook.cpp —— 低级键盘钩子。
//
// 行为规则：
//   * 按下 V 键且物理 Ctrl 按住、Alt 与 Win 未按住时（即 Ctrl+V 或 Ctrl+Shift+V）：
//       - 剪贴板最近一条记录是文本且不超过上限 -> 吞掉本次按键，交给模拟线程逐字打出；
//       - 剪贴板没有记录、不是文本或超限 -> 原样放行，目标程序保持常规粘贴行为。
//   * 模拟进行期间吞掉一切物理 V 按键（无论是否带修饰键）：既忽略重复触发，
//     也避免字面 'v' 混进目标文档——模拟开始会注入 Ctrl/Shift 抬起，用户按住
//     不放的自动重复因此不再携带 Ctrl 状态。窗口覆盖整个模拟过程，直到触发
//     的 V 键松开。
//   * 模拟进行期间按下 Esc -> 请求中止模拟（Esc 本身照常放行给目标程序）。
//   * 修饰键判定使用本钩子自跟踪的“物理”状态（只统计非注入事件）：本软件会
//     注入 Ctrl/Shift 抬起，GetAsyncKeyState 会被注入事件污染——模拟结束后
//     用户仍按住的 Ctrl 在异步键态里是“抬起”，依赖它会漏判。若钩子因系统
//     超时恰好错过某个修饰键事件，物理状态可能失真，重按该键即可恢复。
//   * 本软件自己注入的按键带 LLKHF_INJECTED 标记，直接放行，避免死循环。
#include <windows.h>
#include <atomic>
#include <string>
#include <utility>
#include "hook.h"
#include "typer.h"

namespace {

// 超过此字符数的文本不模拟：动作序列约 80 字节/字符，且以默认速度逐字
// 打出需要数小时。此时放行组合键，让目标程序执行常规粘贴。
constexpr size_t kMaxSimulatedChars = 1000000;

HWND g_hwndOwner = nullptr;
HHOOK g_hHook = nullptr;
// 自有模态 UI（托盘菜单/对话框）打开期间为 true：钩子放行 Ctrl+V，
// 不启动模拟。仅 UI 线程读写（与钩子回调同线程），无并发访问。
bool g_ownModalUIOpen = false;
// 模拟打字功能总开关（托盘菜单「启用」）。UI 线程写、钩子回调读，
// 启动默认开启。
std::atomic<bool> g_enabled{true};
// 模拟启动成功时置位，直到触发的 V 键松开（期间吞掉一切物理 V 按键）。
bool g_swallowVKeyDown = false;
// 物理修饰键状态：只根据非注入事件更新，免疫本软件注入的 keyup 污染。
bool g_physCtrlDown = false;
bool g_physAltDown = false;
bool g_physWinDown = false;

// 读取剪贴板的 CF_UNICODETEXT。取不到文本（无记录、非文本、打不开、超限）返回 false。
bool ClipboardText(std::wstring& out) {
    if (!OpenClipboard(g_hwndOwner)) return false;
    bool hasText = false;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        // GlobalSize 是分配字节上界。剪贴板数据不保证 NUL 终止：先用上界排除
        // 明显超限的分配，再在界内扫描终止符确定实际长度，防止越界读。
        const SIZE_T bytes = GlobalSize(handle);
        if (bytes >= sizeof(wchar_t) &&
            bytes <= (kMaxSimulatedChars + 1) * sizeof(wchar_t)) {
            const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(handle));
            if (data) {
                const SIZE_T maxChars = bytes / sizeof(wchar_t);
                SIZE_T length = 0;
                while (length < maxChars && data[length] != L'\0') {
                    ++length;
                }
                if (length > 0 && length <= kMaxSimulatedChars) {
                    // 唯一可抛点是 assign 的堆分配：单独防护，保证下面的
                    // GlobalUnlock/CloseClipboard 无条件执行（否则剪贴板
                    // 停留在被本进程打开的状态，阻塞其他应用访问）。
                    try {
                        out.assign(data, length);
                        hasText = true;
                    } catch (...) {
                        hasText = false;
                    }
                }
                GlobalUnlock(handle);
            }
        }
    }
    CloseClipboard();
    return hasText;
}

// 根据物理（非注入）键盘事件更新修饰键状态。
void TrackPhysicalModifiers(const KBDLLHOOKSTRUCT& info, WPARAM wParam) {
    const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    switch (info.vkCode) {
    case VK_LCONTROL:
    case VK_RCONTROL:
        g_physCtrlDown = down;
        break;
    case VK_LMENU:
    case VK_RMENU:
        g_physAltDown = down;
        break;
    case VK_LWIN:
    case VK_RWIN:
        g_physWinDown = down;
        if (down && TypingActive()) {
            // 模拟中按下 Win：随后注入的 ←/→ 会被系统当作 Win+←/→ 级联触发
            // 窗口贴靠，立即中止剩余模拟。
            RequestTypingAbort();
        }
        break;
    default:
        break;
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT& info = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (!(info.flags & LLKHF_INJECTED)) {
            TrackPhysicalModifiers(info, wParam);
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            if (info.vkCode == 'V') {
                if (down) {
                    if (TypingActive() || g_swallowVKeyDown) {
                        // 模拟中：吞掉一切物理 V（含自动重复与无修饰的 'v'）。
                        // 置位标志让匹配的 keyup 也被吞掉，不留孤儿 keyup。
                        g_swallowVKeyDown = true;
                        return 1;
                    }
                    // 自有模态 UI 打开期间放行（对话框编辑框里是原生粘贴），
                    // 避免模拟注入落进自己的菜单/对话框。
                    // 功能总开关关闭时同样放行，保持常规粘贴。
                    if (!g_ownModalUIOpen && SimulatedTypingEnabled() &&
                        g_physCtrlDown && !g_physAltDown && !g_physWinDown) {
                        std::wstring text;
                        if (ClipboardText(text) && StartTypingFromText(std::move(text))) {
                            g_swallowVKeyDown = true;
                            return 1; // 吞掉本次组合键，改为模拟打字
                        }
                        // 剪贴板无文本：放行，保持常规粘贴。
                    }
                } else if (g_swallowVKeyDown) {
                    g_swallowVKeyDown = false;
                    return 1;
                }
            } else if (info.vkCode == VK_ESCAPE && down && TypingActive()) {
                // 请求中止本次模拟；Esc 本身照常放行（不吞）。
                RequestTypingAbort();
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace

bool InitKeyboardHook(HWND hwndOwner) {
    g_hwndOwner = hwndOwner;
    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                GetModuleHandleW(nullptr), 0);
    return g_hHook != nullptr;
}

void ShutdownKeyboardHook() {
    if (g_hHook) {
        UnhookWindowsHookEx(g_hHook);
        g_hHook = nullptr;
    }
}

void SetOwnModalUIOpen(bool open) {
    g_ownModalUIOpen = open;
}

void SetSimulatedTypingEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool SimulatedTypingEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}
