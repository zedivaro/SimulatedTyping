// main.cpp —— Simulated Typing 入口：隐藏窗口、托盘图标、菜单与设置。
//
// 软件常驻系统托盘。左键单击托盘图标弹出菜单：
//   * 修改打字速度 —— 对话框输入非负整数（毫秒/键），默认 10，持久化到注册表；
//   * 退出 Simulated Typing。
#include <windows.h>
#include <shellapi.h>
#include "hook.h"
#include "resource.h"
#include "typer.h"

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;      // 托盘回调消息
constexpr int IDM_SPEED = 2001;               // 菜单：修改打字速度
constexpr int IDM_EXIT = 2002;                // 菜单：退出
constexpr wchar_t kMutexName[] = L"Local\\SimulatedTyping.SingleInstance";
constexpr wchar_t kRegistryKey[] = L"Software\\SimulatedTyping";
constexpr wchar_t kRegistryValue[] = L"SpeedMs";
constexpr wchar_t kWindowTitle[] = L"Simulated Typing";

HINSTANCE g_hInst = nullptr;
HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_nid = {};
UINT g_msgTaskbarCreated = 0;
HANDLE g_singleInstanceMutex = nullptr;

// ---------------------------------------------------------------------------
// 速度设置（注册表持久化，HKCU\Software\SimulatedTyping\SpeedMs）
// ---------------------------------------------------------------------------

bool LoadSpeedFromRegistry(unsigned& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD data = 0;
    DWORD size = sizeof(data);
    DWORD type = 0;
    const LRESULT result = RegQueryValueExW(key, kRegistryValue, nullptr, &type,
                                            reinterpret_cast<LPBYTE>(&data), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(DWORD)) {
        return false;
    }
    value = data;
    return true;
}

void SaveSpeedToRegistry(unsigned value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD data = value;
    RegSetValueExW(key, kRegistryValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// “修改打字速度”对话框
// ---------------------------------------------------------------------------

INT_PTR CALLBACK SpeedDialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemInt(dialog, IDC_SPEED_EDIT, g_speedMs.load(std::memory_order_relaxed), FALSE);
        SendDlgItemMessageW(dialog, IDC_SPEED_EDIT, EM_SETLIMITTEXT, 10, 0);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            BOOL translated = FALSE;
            const unsigned value = GetDlgItemInt(dialog, IDC_SPEED_EDIT, &translated, FALSE);
            if (!translated) {
                MessageBoxW(dialog, L"请输入一个非负整数（毫秒/键）。",
                            kWindowTitle, MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            g_speedMs.store(value, std::memory_order_relaxed);
            SaveSpeedToRegistry(value);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;

    default:
        break;
    }
    (void)lParam;
    return FALSE;
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------

void AddTrayIcon() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = static_cast<HICON>(LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP),
                                                IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    lstrcpynW(g_nid.szTip, kWindowTitle,
              sizeof(g_nid.szTip) / sizeof(g_nid.szTip[0]));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    if (g_hwnd) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_SPEED, L"修改打字速度");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出 Simulated Typing");

    SetForegroundWindow(g_hwnd); // 让菜单在点击别处时能正常关闭
    POINT pt = {};
    GetCursorPos(&pt);
    const int command = TrackPopupMenuEx(menu,
                                         TPM_LEFTBUTTON | TPM_RETURNCMD |
                                         TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                                         pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);

    if (command == IDM_SPEED) {
        DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SPEED), g_hwnd, SpeedDialogProc, 0);
    } else if (command == IDM_EXIT) {
        DestroyWindow(g_hwnd);
    }
}

// ---------------------------------------------------------------------------
// 隐藏主窗口
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    if (msg == WM_APP_TRAY) {
        if (lParam == WM_LBUTTONUP) {
            ShowTrayMenu();
        }
        return 0;
    }
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        AddTrayIcon(); // Explorer 重启后恢复托盘图标
        return 0;
    }
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_hInst = hInstance;

    // 单实例保护：同时运行两份会导致按键被模拟两遍。
    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (g_singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Simulated Typing 已在运行。", kWindowTitle,
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    unsigned speed = 10; // 默认 10 毫秒/键
    if (LoadSpeedFromRegistry(speed)) {
        g_speedMs.store(speed, std::memory_order_relaxed);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SimulatedTyping.MainWindow";
    if (!RegisterClassExW(&wc)) return 1;

    // 隐藏窗口：只承载托盘回调与消息循环，从不显示。
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, kWindowTitle,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (!InitTyper()) return 1;
    if (!InitKeyboardHook(g_hwnd)) {
        ShutdownTyper();
        return 1;
    }
    AddTrayIcon();

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTrayIcon();
    ShutdownKeyboardHook();
    ShutdownTyper();
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
    }
    return 0;
}
