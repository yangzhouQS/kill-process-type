/* test_ai_parse.c — WP1 AiExtractJson 单元测试
 * 样本覆盖：围栏 JSON、裸 JSON、嵌套对象/数组、字符串内括号、
 * 无 JSON、截断 JSON、多段 JSON 取首个。
 * 编译：gcc -municode -O2 -Wall -o build\test_ai_parse.exe tests\test_ai_parse.c src\ai.c src\net.c src\process.c src\config.c -lpsapi -liphlpapi
 */
#include "../src/common.h"
#include <stdio.h>
#include <strsafe.h>
#include "../src/ai.h"

static int g_pass = 0, g_fail = 0;

static void T(const WCHAR *label, const WCHAR *input, const WCHAR *expect)
{
    WCHAR *got = AiExtractJson(input);
    BOOL ok;

    if (!expect)
        ok = (got == NULL);
    else
        ok = (got && wcscmp(got, expect) == 0);
    if (ok) {
        g_pass++;
        wprintf(L"  PASS  %ls\n", label);
    } else {
        g_fail++;
        wprintf(L"  FAIL  %ls\n    expect: [%ls]\n    got:    [%ls]\n",
                label, expect ? expect : L"(null)", got ? got : L"(null)");
    }
    free(got);
}

int wmain(void)
{
    wprintf(L"AiExtractJson tests:\n");

    /* 1. 裸 JSON 对象 */
    T(L"bare object", L"answer {\"a\":1} tail", L"{\"a\":1}");

    /* 2. 围栏 json 代码块 */
    T(L"fenced json", L"```json\n[{\"pid\":1}]\n```", L"[{\"pid\":1}]");

    /* 3. 嵌套对象 */
    T(L"nested", L"prefix {\"x\":{\"y\":[1,2]}} suffix", L"{\"x\":{\"y\":[1,2]}}");

    /* 4. 字符串内含 } 不影响配对 */
    T(L"brace in string", L"{\"s\":\"a}b{c\"}", L"{\"s\":\"a}b{c\"}");

    /* 5. 字符串内转义引号 */
    T(L"escaped quote", L"{\"s\":\"he said \\\"hi\\\"\"}", L"{\"s\":\"he said \\\"hi\\\"\"}");

    /* 6. 无 JSON → NULL */
    T(L"no json", L"纯文本没有结构化数据", NULL);

    /* 7. 截断 JSON（depth 未归零）→ NULL */
    T(L"truncated", L"{\"a\":[1,2,", NULL);

    /* 8. 多段 JSON 取首个完整闭合 */
    T(L"first complete", L"{\"a\":1} middle {\"b\":2}", L"{\"a\":1}");

    /* 9. 顶层是数组 */
    T(L"top-level array", L"text [{\"pid\":123,\"level\":\"高\"}] end",
      L"[{\"pid\":123,\"level\":\"高\"}]");

    /* 10. 转义反斜杠后跟引号 */
    T(L"escape backslash", L"{\"path\":\"C:\\\\dir\\\"\"}", L"{\"path\":\"C:\\\\dir\\\"\"}");

    wprintf(L"\nAiTruncateContext tests:\n");
    {
        WCHAR buf[100];
        /* 不超限原样返回 */
        StringCchCopyW(buf, 100, L"short");
        size_t r = AiTruncateContext(buf, 50);
        if (r == 5 && wcscmp(buf, L"short") == 0) {
            g_pass++; wprintf(L"  PASS  no-truncation\n");
        } else { g_fail++; wprintf(L"  FAIL  no-truncation r=%lu\n", (unsigned long)r); }

        /* 超限整行截断+标注 */
        WCHAR big[300];
        for (int i = 0; i < 20; i++)
            StringCchCatW(big, 300, L"line-of-data-abcdefgh\n");
        size_t r2 = AiTruncateContext(big, 100);
        if (r2 < 140 && wcsstr(big, L"已截断") != NULL) {
            g_pass++; wprintf(L"  PASS  truncation+marker r=%lu\n", (unsigned long)r2);
        } else { g_fail++; wprintf(L"  FAIL  truncation r=%lu\n", (unsigned long)r2); }
    }

    wprintf(L"\ntotal: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
