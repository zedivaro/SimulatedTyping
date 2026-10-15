// test_expand.cpp —— keysim 展开与中止清理的单元测试。
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

} // namespace

int main() {
    // 1. 分组结构：Leader（占位 X、←）开头，其后每组对应一个剪贴板字符
    {
        auto groups = ExpandTextGroups(L"abc");
        CHECK(groups.size() == 4);
        if (groups.size() == 4) {
            CHECK(groups[0].kind == ActionGroupKind::Leader && groups[0].actions.size() == 2);
            CHECK(groups[1].kind == ActionGroupKind::Char && groups[1].actions.size() == 1);
            CHECK(groups[2].kind == ActionGroupKind::Char && groups[2].actions.size() == 1);
            CHECK(groups[3].kind == ActionGroupKind::Char && groups[3].actions.size() == 1);
        }
    }

    // 2. Leader 动作：占位 X、方向左键（扩展键）
    {
        auto groups = ExpandTextGroups(L"a");
        CHECK(groups.size() == 2);
        if (groups.size() == 2) {
            const ActionGroup& lead = groups[0];
            CHECK(lead.actions.size() == 2);
            if (lead.actions.size() == 2) {
                CHECK(IsUnicode(lead.actions[0], L'X'));
                CHECK(IsVk(lead.actions[1], VK_LEFT) && lead.actions[1].extended);
            }
        }
    }

    // 3. 字符组：中文、制表符、空格都是单动作 Unicode 注入（字符本体）
    {
        auto g1 = ExpandTextGroups(L"中");
        CHECK(g1.size() == 2);
        if (g1.size() == 2) CHECK(IsUnicode(g1[1].actions[0], L'中'));
        auto g2 = ExpandTextGroups(L"\t");
        CHECK(g2.size() == 2);
        if (g2.size() == 2) CHECK(IsUnicode(g2[1].actions[0], L'\t'));
        auto g3 = ExpandTextGroups(L" ");
        CHECK(g3.size() == 2);
        if (g3.size() == 2) CHECK(IsUnicode(g3[1].actions[0], L' '));
    }

    // 4. 换行组："\r\n" 单组、孤立 '\r' 单组、"\r\r" 两组
    {
        auto a = ExpandTextGroups(L"\r\n");
        CHECK(a.size() == 2);
        if (a.size() == 2) {
            CHECK(a[1].kind == ActionGroupKind::Newline && a[1].actions.size() == 3);
        }
        auto b = ExpandTextGroups(L"\r");
        CHECK(b.size() == 2);
        if (b.size() == 2) CHECK(b[1].kind == ActionGroupKind::Newline);
        auto c = ExpandTextGroups(L"\r\r");
        CHECK(c.size() == 3);
        if (c.size() == 3) {
            CHECK(c[1].kind == ActionGroupKind::Newline && c[2].kind == ActionGroupKind::Newline);
        }
    }

    // 5. 空文本：无任何组（无 Leader，也就没有结束 Delete）
    CHECK(ExpandTextGroups(L"").empty());

    // 6. 代理对：单字符组，码元整体注入
    {
        const wchar_t pair[2] = {0xD852, 0xDF62}; // U+24B62
        auto groups = ExpandTextGroups(std::wstring(pair, 2));
        CHECK(groups.size() == 2);
        if (groups.size() == 2) {
            CHECK(groups[1].kind == ActionGroupKind::Char && groups[1].actions.size() == 1);
            CHECK(groups[1].actions[0].kind == ActionKind::Unicode &&
                  groups[1].actions[0].unitCount == 2);
            if (groups[1].actions[0].unitCount == 2) {
                CHECK(groups[1].actions[0].units[0] == 0xD852);
                CHECK(groups[1].actions[0].units[1] == 0xDF62);
            }
        }
    }

    // 7. 中止清理：Leader 组——X 已打、← 未打时补 Backspace 撤销 X
    {
        auto groups = ExpandTextGroups(L"a");
        CHECK(groups.size() == 2);
        if (groups.size() == 2) {
            const ActionGroup& lead = groups[0];
            auto c1 = AbortCleanup(lead, 1);
            CHECK(c1.size() == 1 && IsVk(c1[0], VK_BACK));
            CHECK(AbortCleanup(lead, 0).empty()); // 组未开始
            CHECK(AbortCleanup(lead, 2).empty()); // ← 已打：占位 X 交给结尾 Delete
        }
    }

    // 8. 中止清理：Newline 组——Space 已打、Ctrl+Backspace 未打时补删 Space
    {
        auto groups = ExpandTextGroups(L"\n");
        CHECK(groups.size() == 2);
        if (groups.size() == 2) {
            const ActionGroup& g = groups[1];
            auto c2 = AbortCleanup(g, 2);
            CHECK(c2.size() == 1 && IsCtrlBackspace(c2[0]));
            CHECK(AbortCleanup(g, 1).empty()); // 只有 Enter：无辅助键残留
            CHECK(AbortCleanup(g, 3).empty()); // 组已完成
        }
    }

    // 9. 中止清理：Char 组是单动作，不存在部分状态
    {
        auto groups = ExpandTextGroups(L"a");
        CHECK(groups.size() == 2);
        if (groups.size() == 2) {
            const ActionGroup& g = groups[1];
            CHECK(AbortCleanup(g, 0).empty());
            CHECK(AbortCleanup(g, 1).empty());
        }
    }

    // 10. 扁平封装：X、← 开头，内容依次展开；无 # 五连、无结尾 Delete
    {
        auto a = ExpandText(L"a");
        CHECK(a.size() == 3);
        if (a.size() == 3) {
            CHECK(IsUnicode(a[0], L'X'));
            CHECK(IsVk(a[1], VK_LEFT));
            CHECK(IsUnicode(a[2], L'a'));
        }
        auto b = ExpandText(L"ab");
        CHECK(b.size() == 4);
        if (b.size() == 4) CHECK(IsUnicode(b[3], L'b'));
    }

    // 11. 扁平顺序：换行规则嵌在内容流中
    {
        auto a = ExpandText(L"a\n");
        CHECK(a.size() == 2 + 1 + 3);
        if (a.size() == 6) {
            CHECK(IsUnicode(a[0], L'X'));
            CHECK(IsVk(a[1], VK_LEFT));
            CHECK(IsUnicode(a[2], L'a'));
            CHECK(IsVk(a[3], VK_RETURN));
            CHECK(IsVk(a[4], VK_SPACE));
            CHECK(IsCtrlBackspace(a[5]));
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
