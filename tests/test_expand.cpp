// test_expand.cpp —— keysim::ExpandText 展开规则的单元测试。
#include <windows.h>
#include <cstdio>
#include <string>
#include "keysim.h"

using namespace keysim;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                \
        }                                                                      \
    } while (0)

bool IsVk(const Action& a, WORD vk) {
    return a.kind == ActionKind::TapVk && a.vk == vk;
}

bool IsUnicode(const Action& a, wchar_t ch) {
    return a.kind == ActionKind::Unicode && a.unitCount == 1 && a.units[0] == ch;
}

bool IsCtrlBackspace(const Action& a) {
    return a.kind == ActionKind::CtrlBackspace;
}

// 普通字符的 # 技巧五连：#、←、字符、→、Backspace。
void CheckCharTrick(const std::vector<Action>& a, wchar_t ch) {
    CHECK(a.size() == 5);
    if (a.size() != 5) return;
    CHECK(IsUnicode(a[0], L'#'));
    CHECK(IsVk(a[1], VK_LEFT));
    CHECK(IsUnicode(a[2], ch));
    CHECK(IsVk(a[3], VK_RIGHT));
    CHECK(IsVk(a[4], VK_BACK));
}

} // namespace

int main() {
    // 1. 普通字符：#、←、字符、→、Backspace
    CheckCharTrick(ExpandText(L"a"), L'a');
    // 2. 中文字符同样走 # 技巧，字符本体以 Unicode 注入
    CheckCharTrick(ExpandText(L"中"), L'中');
    // 3. '#' 自身也只是普通字符
    CheckCharTrick(ExpandText(L"#"), L'#');

    // 4. 换行：Enter、Space、Ctrl+Backspace
    {
        auto a = ExpandText(L"\n");
        CHECK(a.size() == 3);
        if (a.size() == 3) {
            CHECK(IsVk(a[0], VK_RETURN));
            CHECK(IsVk(a[1], VK_SPACE));
            CHECK(IsCtrlBackspace(a[2]));
        }
    }

    // 5. "\r\n" 只算一次换行
    {
        auto a = ExpandText(L"\r\n");
        CHECK(a.size() == 3);
        if (a.size() == 3) CHECK(IsVk(a[0], VK_RETURN));
    }

    // 6. 单独的 '\r'（旧 Mac 行尾）也按换行处理
    {
        auto a = ExpandText(L"\r");
        CHECK(a.size() == 3);
        if (a.size() == 3) {
            CHECK(IsVk(a[0], VK_RETURN));
            CHECK(IsVk(a[1], VK_SPACE));
            CHECK(IsCtrlBackspace(a[2]));
        }
    }

    // 6b. "a\rb"：孤立的 '\r' 按换行展开，不吞内容
    {
        auto a = ExpandText(L"a\rb");
        CHECK(a.size() == 5 + 3 + 5);
        if (a.size() == 5 + 3 + 5) {
            CHECK(IsUnicode(a[2], L'a'));
            CHECK(IsVk(a[5], VK_RETURN));
            CHECK(IsUnicode(a[10], L'b'));
        }
    }

    // 6c. "\r\r"：两个孤立 \r，两次换行
    {
        auto a = ExpandText(L"\r\r");
        CHECK(a.size() == 6);
        if (a.size() == 6) {
            CHECK(IsVk(a[0], VK_RETURN));
            CHECK(IsVk(a[3], VK_RETURN));
        }
    }

    // 6d. "a\r\nb"：CRLF 在文本中间只算一次换行
    {
        auto a = ExpandText(L"a\r\nb");
        CHECK(a.size() == 5 + 3 + 5);
        if (a.size() == 5 + 3 + 5) {
            CHECK(IsUnicode(a[2], L'a'));
            CHECK(IsVk(a[5], VK_RETURN));
            CHECK(IsUnicode(a[10], L'b'));
        }
    }

    // 7. 制表符无特殊处理：作为普通字符走 # 技巧，字符本体是 '\t'
    CheckCharTrick(ExpandText(L"\t"), L'\t');

    // 8. 空文本：无动作
    CHECK(ExpandText(L"").empty());

    // 9. 多字符混合时顺序保持（"a\n" = 5 + 3 个动作）
    {
        auto a = ExpandText(L"a\n");
        CHECK(a.size() == 8);
        if (a.size() == 8) {
            CHECK(IsUnicode(a[2], L'a'));
            CHECK(IsVk(a[5], VK_RETURN));
            CHECK(IsVk(a[6], VK_SPACE));
            CHECK(IsCtrlBackspace(a[7]));
        }
    }

    // 10. UTF-16 代理对被当作一个字符整体注入（仍包装在 # 技巧里）
    {
        const wchar_t pair[2] = {0xD852, 0xDF62}; // U+24B62
        auto a = ExpandText(std::wstring(pair, 2));
        CHECK(a.size() == 5);
        if (a.size() == 5) {
            CHECK(IsUnicode(a[0], L'#'));
            CHECK(IsVk(a[1], VK_LEFT));
            CHECK(a[2].kind == ActionKind::Unicode && a[2].unitCount == 2);
            if (a[2].unitCount == 2) {
                CHECK(a[2].units[0] == 0xD852);
                CHECK(a[2].units[1] == 0xDF62);
            }
            CHECK(IsVk(a[3], VK_RIGHT));
            CHECK(IsVk(a[4], VK_BACK));
        }
    }

    // 11. 孤立高位代理按单个码元处理
    {
        auto a = ExpandText(std::wstring(1, L'\xD852'));
        CHECK(a.size() == 5);
        if (a.size() == 5) {
            CHECK(a[2].kind == ActionKind::Unicode && a[2].unitCount == 1);
            if (a[2].unitCount == 1) CHECK(a[2].units[0] == 0xD852);
        }
    }

    // 12. 方向键标记为扩展键（保持真实键盘的扫描码语义）
    {
        auto a = ExpandText(L"x");
        CHECK(a.size() == 5);
        if (a.size() == 5) {
            CHECK(a[1].kind == ActionKind::TapVk && a[1].vk == VK_LEFT && a[1].extended);
            CHECK(a[3].kind == ActionKind::TapVk && a[3].vk == VK_RIGHT && a[3].extended);
            CHECK(a[4].kind == ActionKind::TapVk && a[4].vk == VK_BACK && !a[4].extended);
        }
    }

    // 13. 孤立低位代理按单个码元处理（覆盖 else 分支）
    {
        auto a = ExpandText(std::wstring(1, L'\xDF62'));
        CHECK(a.size() == 5);
        if (a.size() == 5) {
            CHECK(a[2].kind == ActionKind::Unicode && a[2].unitCount == 1);
            if (a[2].unitCount == 1) CHECK(a[2].units[0] == 0xDF62);
        }
    }

    // 14. 空格自身走五连（Unicode 注入，而非 TapVk VK_SPACE——与 '\t' 一字之差语义不同）
    CheckCharTrick(ExpandText(L" "), L' ');

    // 15. 分组：每组对应一个剪贴板字符（五连/换行三连）
    {
        auto groups = ExpandTextGroups(L"a\n\t");
        CHECK(groups.size() == 3);
        if (groups.size() == 3) {
            CHECK(groups[0].kind == ActionGroupKind::CharTrick && groups[0].actions.size() == 5);
            CHECK(groups[1].kind == ActionGroupKind::Newline && groups[1].actions.size() == 3);
            CHECK(groups[2].kind == ActionGroupKind::CharTrick && groups[2].actions.size() == 5);
            CHECK(groups[2].actions[2].kind == ActionKind::Unicode &&
                  groups[2].actions[2].unitCount == 1 &&
                  groups[2].actions[2].units[0] == L'\t');
        }
    }

    // 16. 代理对为一组，码元整体在第三位
    {
        const wchar_t pair[2] = {0xD852, 0xDF62}; // U+24B62
        auto groups = ExpandTextGroups(std::wstring(pair, 2));
        CHECK(groups.size() == 1);
        if (groups.size() == 1) {
            CHECK(groups[0].kind == ActionGroupKind::CharTrick && groups[0].actions.size() == 5);
            CHECK(groups[0].actions[2].kind == ActionKind::Unicode &&
                  groups[0].actions[2].unitCount == 2);
        }
    }

    // 17. 中止清理：五连各进度的残留 # 清除动作
    {
        auto groups = ExpandTextGroups(L"x");
        CHECK(groups.size() == 1);
        if (groups.size() == 1) {
            const ActionGroup& g = groups[0];
            auto c1 = AbortCleanup(g, 1); // 打完 #：光标在 # 后，直接 Backspace
            CHECK(c1.size() == 1 && IsVk(c1[0], VK_BACK));
            auto c2 = AbortCleanup(g, 2); // 打完 ←：光标在 # 前，先 → 再 Backspace
            CHECK(c2.size() == 2 && IsVk(c2[0], VK_RIGHT) && IsVk(c2[1], VK_BACK));
            auto c3 = AbortCleanup(g, 3); // 字符已打出：光标在 # 前，先 → 再 Backspace
            CHECK(c3.size() == 2 && IsVk(c3[0], VK_RIGHT) && IsVk(c3[1], VK_BACK));
            auto c4 = AbortCleanup(g, 4); // 只差最后的 Backspace
            CHECK(c4.size() == 1 && IsVk(c4[0], VK_BACK));
            CHECK(AbortCleanup(g, 0).empty()); // 组未开始
            CHECK(AbortCleanup(g, 5).empty()); // 组已完成
        }
    }

    // 18. 中止清理：换行组在 Space 之后、Ctrl+Backspace 之前中止，补删 Space
    {
        auto groups = ExpandTextGroups(L"\n");
        CHECK(groups.size() == 1);
        if (groups.size() == 1) {
            const ActionGroup& g = groups[0];
            auto c2 = AbortCleanup(g, 2);
            CHECK(c2.size() == 1 && IsCtrlBackspace(c2[0]));
            CHECK(AbortCleanup(g, 1).empty()); // 只有 Enter：无辅助键残留
            CHECK(AbortCleanup(g, 3).empty()); // 组已完成
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
