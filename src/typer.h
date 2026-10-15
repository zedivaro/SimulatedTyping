// typer.h —— 模拟打字工作线程。
#pragma once
#include <atomic>
#include <string>

// 打字速度（毫秒/键）。由 UI 线程写入、模拟线程读取，默认 10。
extern std::atomic<unsigned> g_speedMs;

// 创建启动事件与工作线程。成功返回 true。
bool InitTyper();

// 通知工作线程退出并等待（至多约 2 秒，防大文本展开期退出挂死）。
void ShutdownTyper();

// 当前是否正在模拟打字。
bool TypingActive();

// 请求中止当前模拟。调用方：键盘钩子（模拟期间检测到物理 Esc 按下），
// 以及托盘菜单打开时的兜底中止（经注入/触摸输入可达）。对无进行中模拟
// 的调用无副作用；标志在下次模拟开始时自动复位。
void RequestTypingAbort();

// 把一段剪贴板文本交给工作线程模拟打字。
// 返回 false 表示当前已在模拟中，本次请求被忽略。
bool StartTypingFromText(std::wstring text);
