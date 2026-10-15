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

bool IsShiftHome(const Action& a) {
    return a.kind == ActionKind::ShiftHome;
}

// 换行组动作布局：
//   [0] Space  [1] Backspace   —— Enter 前击落可能挂起的补全建议
//   [2] Enter
//   [3] Space  [4] Shift+Home  [5] Backspace —— 清除新行自动缩进与辅助空格
const size_t kNewlineActions = 6;

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

    // 3. 字符组：中文、空格是单动作 Unicode 注入；制表符注入真实的 Tab 键
    {
        auto g1 = ExpandTextGroups(L"中");
        CHECK(g1.size() == 2);
        if (g1.size() == 2) CHECK(IsUnicode(g1[1].actions[0], L'中'));
        auto g2 = ExpandTextGroups(L"\t");
        CHECK(g2.size() == 2);
        if (g2.size() == 2) {
            CHECK(g2[1].actions.size() == 1);
            CHECK(IsVk(g2[1].actions[0], VK_TAB) && !g2[1].actions[0].extended);
        }
        auto g3 = ExpandTextGroups(L" ");
        CHECK(g3.size() == 2);
        if (g3.size() == 2) CHECK(IsUnicode(g3[1].actions[0], L' '));
    }

    // 4. 换行组："\r\n" 单组、孤立 '\r' 单组、"\r\r" 两组
    //    换行组动作：Space、Backspace（击落补全）、Enter、Space、
    //    Shift+Home（选中行首空白与辅助空格）、Backspace
    {
        auto a = ExpandTextGroups(L"\r\n");
        CHECK(a.size() == 2);
        if (a.size() == 2) {
            CHECK(a[1].kind == ActionGroupKind::Newline && a[1].actions.size() == kNewlineActions);
            CHECK(IsVk(a[1].actions[0], VK_SPACE) && !a[1].actions[0].extended);
            CHECK(IsVk(a[1].actions[1], VK_BACK) && !a[1].actions[1].extended);
            CHECK(IsVk(a[1].actions[2], VK_RETURN) && !a[1].actions[2].extended);
            CHECK(IsVk(a[1].actions[3], VK_SPACE) && !a[1].actions[3].extended);
            CHECK(IsShiftHome(a[1].actions[4]));
            CHECK(IsVk(a[1].actions[5], VK_BACK) && !a[1].actions[5].extended);
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

    // 8. 中止清理：Newline 组按 6 动作布局逐进度推演
    //    k=1（仅击落 Space）: 补 Backspace 还原行内容，Enter 尚未执行
    //    k=2（击落完成）    : 行已还原，无残留
    //    k=3（Enter 已打）  : 新行是预期内容，无辅助键残留
    //    k=4（辅助 Space）  : 补 Shift+Home + Backspace 清除缩进与空格
    //    k=5（已选中）      : 补 Backspace 删除选区
    {
        auto groups = ExpandTextGroups(L"\n");
        CHECK(groups.size() == 2);
        if (groups.size() == 2) {
            const ActionGroup& g = groups[1];
            auto c1 = AbortCleanup(g, 1);
            CHECK(c1.size() == 1 && IsVk(c1[0], VK_BACK));
            CHECK(AbortCleanup(g, 2).empty());
            CHECK(AbortCleanup(g, 3).empty());
            auto c4 = AbortCleanup(g, 4);
            CHECK(c4.size() == 2 && IsShiftHome(c4[0]) && IsVk(c4[1], VK_BACK));
            auto c5 = AbortCleanup(g, 5);
            CHECK(c5.size() == 1 && IsVk(c5[0], VK_BACK));
            CHECK(AbortCleanup(g, 0).empty());
            CHECK(AbortCleanup(g, 6).empty());
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
        CHECK(a.size() == 2 + 1 + kNewlineActions);
        if (a.size() == 9) {
            CHECK(IsUnicode(a[0], L'X'));
            CHECK(IsVk(a[1], VK_LEFT));
            CHECK(IsUnicode(a[2], L'a'));
            CHECK(IsVk(a[3], VK_SPACE));
            CHECK(IsVk(a[4], VK_BACK));
            CHECK(IsVk(a[5], VK_RETURN));
            CHECK(IsVk(a[6], VK_SPACE));
            CHECK(IsShiftHome(a[7]));
            CHECK(IsVk(a[8], VK_BACK));
        }
    }

    // 12. 结束动作契约：Delete 键、扩展键
    {
        Action d = PlaceholderDeleteAction();
        CHECK(IsVk(d, VK_DELETE) && d.extended);
    }

    // 13. 文本中段的 CRLF 分组与扁平顺序
    {
        auto groups = ExpandTextGroups(L"a\r\nb");
        CHECK(groups.size() == 4);
        if (groups.size() == 4) {
            CHECK(groups[1].kind == ActionGroupKind::Char);
            CHECK(groups[2].kind == ActionGroupKind::Newline &&
                  groups[2].actions.size() == kNewlineActions);
            CHECK(groups[3].kind == ActionGroupKind::Char);
        }
        auto a = ExpandText(L"a\r\nb");
        CHECK(a.size() == 2 + 1 + kNewlineActions + 1);
        if (a.size() == 10) {
            CHECK(IsUnicode(a[2], L'a'));
            CHECK(IsVk(a[3], VK_SPACE));
            CHECK(IsVk(a[4], VK_BACK));
            CHECK(IsVk(a[5], VK_RETURN));
            CHECK(IsShiftHome(a[7]));
            CHECK(IsVk(a[8], VK_BACK));
            CHECK(IsUnicode(a[9], L'b'));
        }
    }

    // 14. 孤立代理按单码元透传（末尾高代理、单独低代理）
    {
        auto g1 = ExpandTextGroups(std::wstring(1, L'\xD852'));
        CHECK(g1.size() == 2);
        if (g1.size() == 2) {
            CHECK(g1[1].actions.size() == 1 && g1[1].actions[0].unitCount == 1 &&
                  g1[1].actions[0].units[0] == 0xD852);
        }
        auto g2 = ExpandTextGroups(std::wstring(1, L'\xDF62'));
        CHECK(g2.size() == 2);
        if (g2.size() == 2) {
            CHECK(g2[1].actions.size() == 1 && g2[1].actions[0].unitCount == 1 &&
                  g2[1].actions[0].units[0] == 0xDF62);
        }
    }

    // 15. extended 负向断言：Enter/Space/Backspace 不得误标扩展键
    {
        auto a = ExpandText(L"\n");
        CHECK(a.size() == 2 + kNewlineActions);
        if (a.size() == 8) {
            CHECK(IsVk(a[2], VK_SPACE) && !a[2].extended);
            CHECK(IsVk(a[3], VK_BACK) && !a[3].extended);
            CHECK(IsVk(a[4], VK_RETURN) && !a[4].extended);
            CHECK(IsVk(a[5], VK_SPACE) && !a[5].extended);
        }
        auto groups = ExpandTextGroups(L"x");
        CHECK(groups.size() == 2);
        if (groups.size() == 2) {
            // Backspace 只出现在 Leader 组的中止清理里，同样不得标扩展键
            auto c = AbortCleanup(groups[0], 1);
            CHECK(c.size() == 1 && IsVk(c[0], VK_BACK) && !c[0].extended);
        }
    }

    // 16. 制表符混在串中的顺序
    {
        auto a = ExpandText(L"a\tb");
        CHECK(a.size() == 5);
        if (a.size() == 5) {
            CHECK(IsUnicode(a[2], L'a'));
            CHECK(IsVk(a[3], VK_TAB));
            CHECK(IsUnicode(a[4], L'b'));
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
