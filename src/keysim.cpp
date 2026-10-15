// keysim.cpp —— ExpandTextGroups / AbortCleanup / ExpandText 的实现。
#include "keysim.h"

namespace keysim {

namespace {

bool IsExtendedVk(WORD vk) {
    return vk == VK_DELETE || vk == VK_LEFT || vk == VK_RIGHT ||
           vk == VK_UP || vk == VK_DOWN;
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

// 开头占位组：先注入 X，再把光标移到 X 左侧。此后所有内容字符都插入在
// X 之前（把 X 不断推向右），首个内容键因此不再是行首，编辑器不会弹出
// 自动补全；结束时注入 Delete 即可删掉光标右侧的 X。
ActionGroup LeaderGroup() {
    ActionGroup group;
    group.kind = ActionGroupKind::Leader;
    group.actions.push_back(Unicode1(L'X'));
    group.actions.push_back(TapVk(VK_LEFT));
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

ActionGroup CharGroup(const Action& character) {
    ActionGroup group;
    group.kind = ActionGroupKind::Char;
    group.actions.push_back(character);
    return group;
}

} // namespace

std::vector<ActionGroup> ExpandTextGroups(const std::wstring& text) {
    std::vector<ActionGroup> groups;
    if (text.empty()) {
        return groups; // 无内容：无占位、无结束 Delete
    }
    groups.push_back(LeaderGroup());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
            // "\r\n" 只按一次换行处理（换行由后面的 '\n' 触发）。
            continue;
        }
        if (ch == L'\r' || ch == L'\n') {
            // 换行（含孤立的 '\r'，即旧 Mac 行尾）。
            groups.push_back(NewlineGroup());
        } else if (ch == L'\t') {
            // 制表符：Unicode 注入的 \t 在不少编辑器里不触发缩进，
            // 注入真实的 Tab 键。
            groups.push_back(CharGroup(TapVk(VK_TAB)));
        } else if (IS_HIGH_SURROGATE(ch) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) {
            // 代理对是一个码位，作为整体注入。
            groups.push_back(CharGroup(Unicode2(ch, text[i + 1])));
            ++i;
        } else {
            groups.push_back(CharGroup(Unicode1(ch)));
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
    case ActionGroupKind::Leader:
        // 只打了 X、还没打 ←（光标在 X 右侧）：Backspace 撤销 X。
        // ← 已打时占位 X 交给结尾的 Delete，无需清理。
        out.push_back(TapVk(VK_BACK));
        break;
    case ActionGroupKind::Newline:
        // Enter 与 Space 已注入而 Ctrl+Backspace 未注入时，Space 是辅助键，
        // 补上删除动作；只有 Enter 时没有辅助键残留。
        if (completed == 2) {
            out.push_back(CtrlBackspace());
        }
        break;
    case ActionGroupKind::Char:
        // 单动作组，不存在部分状态。
        break;
    }
    return out;
}

Action PlaceholderDeleteAction() {
    return TapVk(VK_DELETE);
}

std::vector<Action> ExpandText(const std::wstring& text) {
    std::vector<Action> out;
    for (const ActionGroup& group : ExpandTextGroups(text)) {
        out.insert(out.end(), group.actions.begin(), group.actions.end());
    }
    return out;
}

}
