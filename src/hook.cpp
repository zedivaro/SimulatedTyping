// hook.cpp —— 低级键盘钩子。
//
// 行为规则：
//   * 按下 V 键且 Ctrl 按住、Alt 未按住时（即 Ctrl+V 或 Ctrl+Shift+V）：
//       - 剪贴板最近一条记录是文本 -> 吞掉本次按键，交给模拟线程逐字打出；
//       - 剪贴板没有记录或不是文本 -> 原样放行，目标程序保持常规粘贴行为；
//       - 正在模拟中 -> 吞掉并忽略（不支持排队）。
//   * 本软件自己注入的按键带 LLKHF_INJECTED 标记，直接放行，避免死循环。
//   * 触发模拟时，匹配的 V 键 keyup 一并吞掉。
#include <windows.h>
#include <string>
#include <utility>
#include "hook.h"
#include "typer.h"

namespace {

HWND g_hwndOwner = nullptr;
HHOOK g_hHook = nullptr;
bool g_swallowVKeyUp = false;

// 读取剪贴板的 CF_UNICODETEXT。取不到文本（无记录、非文本、打不开）返回 false。
bool ClipboardText(std::wstring& out) {
    if (!OpenClipboard(g_hwndOwner)) return false;
    bool hasText = false;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
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
                const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                if (ctrl && !alt) {
                    if (TypingActive()) {
                        return 1; // 模拟中：忽略重复触发
                    }
                    std::wstring text;
                    if (ClipboardText(text) && StartTypingFromText(std::move(text))) {
                        g_swallowVKeyUp = true;
                        return 1; // 吞掉本次组合键，改为模拟打字
                    }
                    // 剪贴板无文本：放行，保持常规粘贴。
                }
            } else if (up && g_swallowVKeyUp) {
                g_swallowVKeyUp = false;
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
