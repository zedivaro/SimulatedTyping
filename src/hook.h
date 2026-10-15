// hook.h —— 低级键盘与鼠标钩子。
#pragma once
#include <windows.h>

// 在调用线程上安装 WH_KEYBOARD_LL 与 WH_MOUSE_LL 钩子（该线程必须运行
// 消息循环）。任一安装失败即回滚并返回 false。
bool InitInputHooks(HWND hwndOwner);

// 卸载键盘与鼠标钩子（与 InitInputHooks 配对）。
void ShutdownInputHooks();

// 通知钩子：自有模态 UI（托盘菜单/速度对话框）当前打开。
// 打开期间 Ctrl+V 放行（对话框内是原生粘贴），不启动模拟，
// 避免注入落进自己的 UI。
void SetOwnModalUIOpen(bool open);

// 模拟打字功能总开关（托盘菜单「启用」勾选项）。关闭时 Ctrl+V 原样放行；
// 启动默认为开启。
void SetSimulatedTypingEnabled(bool enabled);
bool SimulatedTypingEnabled();
