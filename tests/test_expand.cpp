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

    // 6. 单独的 '\r' 不产生任何动作
    CHECK(ExpandText(L"\r").empty());

    // 7. 制表符：连续四个 Space
    {
        auto a = ExpandText(L"\t");
        CHECK(a.size() == 4);
        for (const Action& act : a) CHECK(IsVk(act, VK_SPACE));
    }

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
            CHECK(IsVk(a[1], VK_LEFT));
            CHECK(a[2].kind == ActionKind::Unicode && a[2].unitCount == 2);
            if (a[2].unitCount == 2) {
                CHECK(a[2].units[0] == 0xD852);
                CHECK(a[2].units[1] == 0xDF62);
            }
            CHECK(IsVk(a[3], VK_RIGHT));
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

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
