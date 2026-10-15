// keysim.h —— 把剪贴板文本展开成“模拟按键动作”序列的纯逻辑。
//
// 展开规则（用于规避目标编辑器的自动补全）：
//   换行 '\n'  -> Enter、Space、Ctrl+Backspace
//   其他字符（含 '\t' 等一切非换行字符）-> '#'、方向左键、该字符、方向右键、
//   Backspace，五连包装
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

enum class ActionGroupKind {
    CharTrick,  // 普通字符的五连：#、←、字符、→、Backspace
    Newline,    // 换行：Enter、Space、Ctrl+Backspace
};

// 一组动作对应一个剪贴板字符。模拟按组推进；组内动作之间可以中止，
// 中止后由 AbortCleanup 给出清除残留辅助键所需的补注动作。
struct ActionGroup {
    ActionGroupKind kind = ActionGroupKind::CharTrick;
    std::vector<Action> actions;
};

// 将一段文本展开为按键动作组（每组对应一个剪贴板字符）。纯函数，无副作用。
std::vector<ActionGroup> ExpandTextGroups(const std::wstring& text);

// 中止清理：某组动作执行到第 completed 个完成后被中止时，为清除残留的
// 辅助键（未删掉的 #、换行后未删的 Space）还应立即注入的动作。
// 已打出的内容字符不属于残留，不会被清理。纯函数，无副作用。
std::vector<Action> AbortCleanup(const ActionGroup& group, size_t completed);

// 便捷封装：展开为扁平动作序列。
std::vector<Action> ExpandText(const std::wstring& text);

}
