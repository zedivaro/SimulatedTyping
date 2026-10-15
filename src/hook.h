// hook.h —— 低级键盘钩子：截获 Ctrl+V / Ctrl+Shift+V。
#pragma once
#include <windows.h>

// 在调用线程上安装 WH_KEYBOARD_LL 钩子（该线程必须运行消息循环）。
bool InitKeyboardHook(HWND hwndOwner);

// 卸载钩子。
void ShutdownKeyboardHook();

// 通知钩子：自有模态 UI（托盘菜单/速度对话框）当前打开。
// 打开期间 Ctrl+V 放行（对话框内是原生粘贴），不启动模拟，
// 避免注入落进自己的 UI。
void SetOwnModalUIOpen(bool open);

// 模拟打字功能总开关（托盘菜单「启用」勾选项）。关闭时 Ctrl+V 原样放行；
// 启动默认为开启。
void SetSimulatedTypingEnabled(bool enabled);
bool SimulatedTypingEnabled();
