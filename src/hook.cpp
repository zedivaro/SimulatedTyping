// hook.cpp —— 低级键盘钩子。
//
// 行为规则：
//   * 按下 V 键且 Ctrl 按住、Alt 未按住时（即 Ctrl+V 或 Ctrl+Shift+V）：
//       - 剪贴板最近一条记录是文本 -> 吞掉本次按键，交给模拟线程逐字打出；
//       - 剪贴板没有记录或不是文本 -> 原样放行，目标程序保持常规粘贴行为；
//       - 正在模拟中 -> 吞掉并忽略（不支持排队）。
//   * 本软件自己注入的按键带 LLKHF_INJECTED 标记，直接放行，避免死循环。
//   * 触发模拟后到 V 键松开前，吞掉一切物理 V 按键（模拟开始会注入 Ctrl
//     抬起，自动重复不再携带 Ctrl，放行会把字面 'v' 漏进目标文档）。
#include <windows.h>
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
// 触发模拟后为 true，直到匹配的 V 键松开。期间吞掉一切物理 V 按键：
// 模拟开始时会注入 Ctrl 抬起，用户按住 Ctrl+V 触发的自动重复不再携带
// Ctrl 状态，若放行会把字面 'v' 漏进目标文档。
bool g_swallowVKeyDown = false;

// 读取剪贴板的 CF_UNICODETEXT。取不到文本（无记录、非文本、打不开）返回 false。
bool ClipboardText(std::wstring& out) {
    if (!OpenClipboard(g_hwndOwner)) return false;
    bool hasText = false;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle && GlobalSize(handle) <= kMaxSimulatedChars * sizeof(wchar_t)) {
        const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(handle));
        if (data) {
            out.assign(data);
            GlobalUnlock(handle);
            hasText = !out.empty();
        }
    }
    CloseClipboard();
    return hasText;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (!(info->flags & LLKHF_INJECTED) && info->vkCode == 'V') {
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            if (down) {
                if (TypingActive() || g_swallowVKeyDown) {
                    return 1; // 模拟中：吞掉（含按住不放产生的自动重复）
                }
                const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                if (ctrl && !alt) {
                    std::wstring text;
                    if (ClipboardText(text) && StartTypingFromText(std::move(text))) {
                        g_swallowVKeyDown = true;
                        return 1; // 吞掉本次组合键，改为模拟打字
                    }
                    // 剪贴板无文本：放行，保持常规粘贴。
                }
            } else if (up && g_swallowVKeyDown) {
                g_swallowVKeyDown = false;
                return 1;
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
