// keysim.h —— 把剪贴板文本展开成“模拟按键动作”序列的纯逻辑。
//
// 展开规则（用于规避目标编辑器的自动补全）：
//   换行 '\n'  -> Enter、Space、Ctrl+Backspace
//   制表 '\t'  -> 连续四个 Space
//   其他字符   -> '#'、方向左键、该字符、方向右键、Backspace
// 其中 "\r\n" 中的 '\r' 被跳过（只算一次换行），孤立的 '\r'（旧 Mac 行尾）
// 也按一次换行处理；UTF-16 代理对被当作一个字符整体注入。
#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace keysim {

enum class ActionKind {
    TapVk,          // 按下并抬起一个虚拟键
    Unicode,        // 以 KEYEVENTF_UNICODE 注入 1~2 个 UTF-16 码元
    CtrlBackspace,  // 按住 Ctrl 并按下 Backspace（复合动作）
};

struct Action {
    ActionKind kind = ActionKind::TapVk;
    WORD vk = 0;          // kind == TapVk 时有效
    bool extended = false; // 该虚拟键是否为扩展键（方向键等）
    wchar_t units[2] = {}; // kind == Unicode 时的码元
    int unitCount = 0;     // 1 或 2
};

// 将一段文本展开为按键动作序列。纯函数，无副作用。
std::vector<Action> ExpandText(const std::wstring& text);

}
