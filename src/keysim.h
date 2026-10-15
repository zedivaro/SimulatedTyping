// keysim.h —— 把剪贴板文本展开成“模拟按键动作”序列的纯逻辑。
// 展开规则（用于规避目标编辑器的自动补全）：
//   开始 -> 注入占位字符 X，再把光标移到 X 左侧（Leader 组）；
//   换行 '\n' -> Enter、Space、Shift+Ctrl+←（选中辅助空格）、Backspace；
//   制表 '\t' -> 真实的 Tab 键（Unicode 注入的 \t 在部分编辑器里无效）；
//   其他字符 -> 字符本体（Unicode 注入）；
//   结束/中止 -> 由执行方注入 Delete，删除光标右侧的占位 X。
// 内容字符插在占位 X 之前，首个内容键因此不再是行首，编辑器不会弹出
// 自动补全。其中 "\r\n" 中的 '\r' 被跳过（只算一次换行），孤立的 '\r'
// （旧 Mac 行尾）也按一次换行处理；UTF-16 代理对被当作一个字符整体注入。
#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace keysim {

enum class ActionKind {
    TapVk,          // 按下并抬起一个虚拟键
    Unicode,        // 以 KEYEVENTF_UNICODE 注入 1~2 个 UTF-16 码元
    SelectWordLeft, // 按住 Shift+Ctrl 并按 ←，再抬起（选中光标左侧内容）
};

struct Action {
    ActionKind kind = ActionKind::TapVk;
    WORD vk = 0;          // kind == TapVk 时有效
    bool extended = false; // 该虚拟键是否为扩展键（方向键等）
    wchar_t units[2] = {}; // kind == Unicode 时的码元
    int unitCount = 0;     // 1 或 2
};

enum class ActionGroupKind {
    Leader,   // 开头占位：X、←。完成后由执行方在结束/中止时补注 Delete。
    Newline,  // 换行：Enter、Space、Shift+Ctrl+←、Backspace
    Char,     // 普通字符：字符本体的 Unicode 注入（单动作，无部分状态）
};

// 一组动作。Leader/Newline 组含多个动作，组内动作之间可以中止，中止后
// 由 AbortCleanup 给出清除残留辅助键所需的补注动作；Char 组是单动作。
struct ActionGroup {
    ActionGroupKind kind = ActionGroupKind::Leader;
    std::vector<Action> actions;
};

// 将一段文本展开为按键动作组：首组为 Leader（文本非空时），随后每组对应
// 一个剪贴板字符。结尾的 Delete 不在展开结果中——由执行方在模拟结束或
// 中止时（Leader 已完成的前提下）注入。纯函数，无副作用。
std::vector<ActionGroup> ExpandTextGroups(const std::wstring& text);

// 中止清理：某组动作执行到第 completed 个完成后被中止时，为清除残留的
// 辅助键（未撤销的占位 X、换行后未删的 Space）还应立即注入的动作。
// 已打出的内容字符不属于残留，不会被清理。纯函数，无副作用。
std::vector<Action> AbortCleanup(const ActionGroup& group, size_t completed);

// 结束动作：删除光标右侧占位字符 X 的 Delete 键。执行方在模拟结束或
// 中止（且 Leader 组已完成）时注入。
Action PlaceholderDeleteAction();

// 便捷封装：展开为扁平动作序列（不含结尾 Delete）。
std::vector<Action> ExpandText(const std::wstring& text);

}
