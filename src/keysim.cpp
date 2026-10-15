// keysim.cpp —— ExpandText 的实现。
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
void AppendCharTrick(std::vector<Action>& out, const Action& character) {
    out.push_back(Unicode1(L'#'));
    out.push_back(TapVk(VK_LEFT));
    out.push_back(character);
    out.push_back(TapVk(VK_RIGHT));
    out.push_back(TapVk(VK_BACK));
}

} // namespace

std::vector<Action> ExpandText(const std::wstring& text) {
    std::vector<Action> out;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r') {
            // 剪贴板中的 "\r\n" 只按一次换行处理。
            continue;
        }
        if (ch == L'\n') {
            out.push_back(TapVk(VK_RETURN));
            out.push_back(TapVk(VK_SPACE));
            out.push_back(CtrlBackspace());
        } else if (ch == L'\t') {
            for (int k = 0; k < 4; ++k) {
                out.push_back(TapVk(VK_SPACE));
            }
        } else if (IS_HIGH_SURROGATE(ch) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) {
            // 代理对是一个码位，作为整体包装进同一套 # 技巧。
            AppendCharTrick(out, Unicode2(ch, text[i + 1]));
            ++i;
        } else {
            AppendCharTrick(out, Unicode1(ch));
        }
    }
    return out;
}

}
