// hook.h —— 低级键盘钩子：截获 Ctrl+V / Ctrl+Shift+V。
#pragma once
#include <windows.h>

// 在调用线程上安装 WH_KEYBOARD_LL 钩子（该线程必须运行消息循环）。
bool InitKeyboardHook(HWND hwndOwner);

// 卸载钩子。
void ShutdownKeyboardHook();
