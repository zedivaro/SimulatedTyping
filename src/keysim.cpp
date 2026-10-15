// keysim.cpp —— ExpandTextGroups / AbortCleanup / ExpandText 的实现。
#include "keysim.h"

namespace keysim {

namespace {

bool IsExtendedVk(WORD vk) {
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
}

Action TapVk(WORD vk) {
    Action a;
    a.kind = ActionKind::TapVk;
    a.vk = vk;
    a.extended = IsExtendedVk(vk);
    return a;
}

Action CtrlBackspace() {
    Action a;
    a.kind = ActionKind::CtrlBackspace;
    return a;
}

Action Unicode1(wchar_t unit) {
    Action a;
    a.kind = ActionKind::Unicode;
    a.units[0] = unit;
    a.unitCount = 1;
    return a;
}

Action Unicode2(wchar_t high, wchar_t low) {
    Action a;
    a.kind = ActionKind::Unicode;
    a.units[0] = high;
    a.units[1] = low;
    a.unitCount = 2;
    return a;
}

// 其他字符的防自动补全五连：#、←、字符、→、Backspace。
// 目标编辑器看到的是 "#" 与字符先后输入且中间有光标移动，从而不会弹出补全。
ActionGroup CharTrickGroup(const Action& character) {
    ActionGroup group;
    group.kind = ActionGroupKind::CharTrick;
    group.actions.push_back(Unicode1(L'#'));
    group.actions.push_back(TapVk(VK_LEFT));
    group.actions.push_back(character);
    group.actions.push_back(TapVk(VK_RIGHT));
    group.actions.push_back(TapVk(VK_BACK));
    return group;
}

ActionGroup NewlineGroup() {
    ActionGroup group;
    group.kind = ActionGroupKind::Newline;
    group.actions.push_back(TapVk(VK_RETURN));
    group.actions.push_back(TapVk(VK_SPACE));
    group.actions.push_back(CtrlBackspace());
    return group;
}

} // namespace

std::vector<ActionGroup> ExpandTextGroups(const std::wstring& text) {
    std::vector<ActionGroup> groups;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
            // "\r\n" 只按一次换行处理（换行由后面的 '\n' 触发）。
            continue;
        }
        if (ch == L'\r' || ch == L'\n') {
            // 换行（含孤立的 '\r'，即旧 Mac 行尾）。
            groups.push_back(NewlineGroup());
        } else if (IS_HIGH_SURROGATE(ch) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) {
            // 代理对是一个码位，作为整体包装进同一套 # 技巧。
            groups.push_back(CharTrickGroup(Unicode2(ch, text[i + 1])));
            ++i;
        } else {
            groups.push_back(CharTrickGroup(Unicode1(ch)));
        }
    }
    return groups;
}

std::vector<Action> AbortCleanup(const ActionGroup& group, size_t completed) {
    std::vector<Action> out;
    const size_t total = group.actions.size();
    if (completed == 0 || completed >= total) {
        return out; // 组未开始或已完成：无残留
    }
    switch (group.kind) {
    case ActionGroupKind::CharTrick:
        // 残留的是还没删掉的 '#'（字符本体不属于残留）：
        //   完成到第 1 个（光标在 # 后）          -> Backspace
        //   完成到第 2/3 个（光标在 # 前/字符与 # 之间）-> 先 → 越过 # 再 Backspace
        //   完成到第 4 个（光标在 # 后）          -> Backspace
        if (completed == 2 || completed == 3) {
            out.push_back(TapVk(VK_RIGHT));
        }
        out.push_back(TapVk(VK_BACK));
        break;
    case ActionGroupKind::Newline:
        // Enter 与 Space 已注入而 Ctrl+Backspace 未注入时，Space 是辅助键，
        // 补上删除动作；只有 Enter 时没有辅助键残留。
        if (completed == 2) {
            out.push_back(CtrlBackspace());
        }
        break;
    }
    return out;
}

std::vector<Action> ExpandText(const std::wstring& text) {
    std::vector<Action> out;
    for (const ActionGroup& group : ExpandTextGroups(text)) {
        out.insert(out.end(), group.actions.begin(), group.actions.end());
    }
    return out;
}

}
