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
constexpr UINT kTrayIconId = 1;               // 托盘图标 ID（与 NIM_DELETE 一致）
constexpr wchar_t kMutexName[] = L"Local\\SimulatedTyping.SingleInstance";
constexpr wchar_t kRegistryKey[] = L"Software\\SimulatedTyping";
constexpr wchar_t kRegistryValue[] = L"SpeedMs";
constexpr wchar_t kWindowTitle[] = L"Simulated Typing";

HINSTANCE g_hInst = nullptr;
HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_nid = {};
UINT g_msgTaskbarCreated = 0;
HANDLE g_singleInstanceMutex = nullptr;

// 致命初始化失败：托盘尚未建立，只能用消息框告知后退出。
// TOPMOST 防止被模态对话框遮挡导致用户看不到退出原因。
void NotifyFatal(const wchar_t* message) {
    MessageBoxW(nullptr, message, kWindowTitle,
                MB_OK | MB_ICONERROR | MB_TOPMOST);
}

// ---------------------------------------------------------------------------
// 速度设置（注册表持久化，HKCU\Software\SimulatedTyping\SpeedMs）
// ---------------------------------------------------------------------------

// 超过此值的速度视为输入错误（毫秒/键）。注册表被外部改成异常大值时启动加载也会夹紧。
constexpr unsigned kMaxSpeedMs = 3600000; // 1 小时/键

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
            if (value > kMaxSpeedMs) {
                // 极大值会让单步 Sleep 长达数日，Esc 中止形同虚设。
                MessageBoxW(dialog, L"速度过大：上限为 3600000 毫秒/键。",
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

bool AddTrayIcon() {
    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon); // TaskbarCreated 重加图标前先释放旧句柄
        g_nid.hIcon = nullptr;
    }
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = kTrayIconId;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    // 按系统小图标尺寸加载，避免高 DPI 下任务栏内缩放发糊。
    g_nid.hIcon = static_cast<HICON>(LoadImageW(
        g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!g_nid.hIcon) return false;
    lstrcpynW(g_nid.szTip, kWindowTitle,
              sizeof(g_nid.szTip) / sizeof(g_nid.szTip[0]));
    return Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

void RemoveTrayIcon() {
    if (g_hwnd) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}

void ShowTrayMenu() {
    static bool g_trayMenuOpen = false;
    if (g_trayMenuOpen) {
        return; // 菜单/对话框打开期间，模态循环仍会派发托盘回调，防止重入
    }
    if (TypingActive()) {
        // 防止剩余注入落进自己的菜单/对话框（文本中的 Enter 会被模态循环
        // 翻译成默认按钮，可能把剪贴板内容当作速度写进注册表）。
        RequestTypingAbort();
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    g_trayMenuOpen = true;
    SetOwnModalUIOpen(true); // 期间 Ctrl+V 原生粘贴，不启动模拟
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
    g_trayMenuOpen = false; // 覆盖整个菜单+对话框期间（清零必须晚于模态对话框）
    SetOwnModalUIOpen(false);
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
        if (!AddTrayIcon()) {
            // Explorer 重启后重加失败：无图标常驻会变成不可控僵尸（还占着
            // 单实例互斥体，用户重启软件会被“已在运行”挡住），直接退出。
            NotifyFatal(L"无法恢复托盘图标，程序即将退出。");
            PostQuitMessage(0);
        }
        return 0;
    }
    switch (msg) {
    case WM_DESTROY:
        RemoveTrayIcon(); // 窗口销毁前移除图标（NIM_DELETE 依赖 hWnd 存活）
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
    if (!g_singleInstanceMutex) {
        NotifyFatal(L"无法创建单实例互斥体，程序即将退出。");
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Simulated Typing 已在运行。", kWindowTitle,
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    unsigned speed = 10; // 默认 10 毫秒/键
    if (LoadSpeedFromRegistry(speed) && speed > kMaxSpeedMs) {
        // 注册表被外部改成异常大值时夹紧到上限（与对话框一致）。
        speed = kMaxSpeedMs;
    }
    g_speedMs.store(speed, std::memory_order_relaxed);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SimulatedTyping.MainWindow";
    if (!RegisterClassExW(&wc)) {
        NotifyFatal(L"窗口类注册失败，程序即将退出。");
        return 1;
    }

    // 隐藏窗口：只承载托盘回调与消息循环，从不显示。
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, kWindowTitle,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) {
        NotifyFatal(L"窗口创建失败，程序即将退出。");
        return 1;
    }

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (!InitTyper()) {
        NotifyFatal(L"模拟线程创建失败，程序即将退出。");
        return 1;
    }
    if (!InitKeyboardHook(g_hwnd)) {
        ShutdownTyper();
        NotifyFatal(L"键盘钩子安装失败（可能被安全策略限制），程序即将退出。");
        return 1;
    }
    if (!AddTrayIcon()) {
        // 托盘图标是唯一的交互面：加不上就成了无法控制的常驻进程，必须退出。
        ShutdownKeyboardHook();
        ShutdownTyper();
        if (g_nid.hIcon) {
            DestroyIcon(g_nid.hIcon); // LoadImageW 已加载的句柄随失败路径释放
            g_nid.hIcon = nullptr;
        }
        NotifyFatal(L"无法创建托盘图标，程序即将退出。");
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownTyper();          // 先停模拟（吞 V 保护覆盖整个收尾期），再卸钩子
    ShutdownKeyboardHook();
    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = nullptr;
    }
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
    }
    return 0;
}
