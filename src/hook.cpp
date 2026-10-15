// hook.cpp —— 低级键盘与鼠标钩子。
//
// 行为规则：
//   * 按下 V 键且物理 Ctrl 按住、Alt 与 Win 未按住时（即 Ctrl+V 或 Ctrl+Shift+V）：
//       - 当前剪贴板内容是文本且不超过上限 -> 吞掉本次按键，交给模拟线程逐字打出；
//       - 剪贴板没有文本或超限 -> 原样放行，目标程序保持常规粘贴行为。
//   * 模拟进行期间屏蔽一切物理键盘与鼠标事件（含鼠标移动、点击、滚轮），
//     防止杂散输入混进目标文档。Esc 也被吞掉，仅由本软件消费用于中止模拟，
//     不传达给目标程序。
//   * 模拟结束后触发键仍按住时，吞掉其自动重复直到松开：模拟开始会注入
//     Ctrl/Shift 抬起，自动重复不再携带 Ctrl 状态，放行会把字面 'v' 漏进
//     目标文档。
//   * 修饰键判定使用本钩子自跟踪的“物理”状态（只统计非注入事件）：本软件会
//     注入 Ctrl/Shift 抬起，GetAsyncKeyState 会被注入事件污染——模拟结束后
//     用户仍按住的 Ctrl 在异步键态里是“抬起”，依赖它会漏判。若钩子因系统
//     超时恰好错过某个修饰键事件，物理状态可能失真，重按该键即可恢复。
//   * 本软件自己注入的按键带 LLKHF_INJECTED / LLMHF_INJECTED 标记，直接放行，
//     避免死循环。
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
HHOOK g_hKeyboardHook = nullptr;
HHOOK g_hMouseHook = nullptr;
// 自有模态 UI（托盘菜单/对话框）打开期间为 true：钩子放行 Ctrl+V，
// 不启动模拟。仅 UI 线程读写（与钩子回调同线程），无并发访问。
bool g_ownModalUIOpen = false;
// 模拟打字功能总开关（托盘菜单「启用」）。UI 线程写、钩子回调读，
// 启动默认开启。
std::atomic<bool> g_enabled{true};
// 模拟启动成功时置位，直到触发的 V 键松开——无论松开发生在模拟期间（兜底
// 屏蔽分支中清除）还是模拟结束之后。残留未清会导致下一次按 V 被误吞。
bool g_swallowVKeyDown = false;
// 物理修饰键状态：只根据非注入事件更新，免疫本软件注入的 keyup 污染。
bool g_physCtrlDown = false;
bool g_physAltDown = false;
bool g_physWinDown = false;

// 读取剪贴板的 CF_UNICODETEXT。取不到文本（无文本、打不开、超限）返回 false。
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
            if (TypingActive()) {
                // 模拟中：屏蔽一切物理键盘事件，包括 Esc。Esc 由本软件消费
                // 用于中止模拟，不传达给目标程序。
                if (info.vkCode == VK_ESCAPE && down) RequestTypingAbort();
                // 模拟中松开触发键：必须清除吞键标志，否则模拟结束后标志
                // 残留，下一次按键（含字母 v、新的 Ctrl+V）会被误吞一次。
                if (info.vkCode == 'V' && !down) g_swallowVKeyDown = false;
                return 1;
            } else if (info.vkCode == 'V') {
                if (down) {
                    if (g_swallowVKeyDown) {
                        return 1; // 触发键仍按住：吞掉自动重复
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
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT& info = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (!(info.flags & LLMHF_INJECTED) && TypingActive()) {
            // 模拟中：屏蔽一切物理鼠标事件（移动、按键、滚轮）。
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace

bool InitInputHooks(HWND hwndOwner) {
    g_hwndOwner = hwndOwner;
    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                        GetModuleHandleW(nullptr), 0);
    if (!g_hKeyboardHook) return false;
    g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                     GetModuleHandleW(nullptr), 0);
    if (!g_hMouseHook) {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = nullptr;
        return false;
    }
    return true;
}

void ShutdownInputHooks() {
    if (g_hMouseHook) {
        UnhookWindowsHookEx(g_hMouseHook);
        g_hMouseHook = nullptr;
    }
    if (g_hKeyboardHook) {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = nullptr;
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
