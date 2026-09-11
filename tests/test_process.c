/* test_process.c - process 模块验证（仅枚举打印，不杀进程）
 * 编译: gcc -municode -O2 -Wall -o build\test_process.exe tests\test_process.c src\process.c -lpsapi
 */
#include "../src/common.h"
#include <stdio.h>
#include "../src/process.h"

static const WCHAR *TypeTag(ProcType t)
{
    switch (t) {
    case PT_NODE:   return L"NODE";
    case PT_PYTHON: return L"PY";
    default:        return L"?";
    }
}

/* 分类规则单元自检 */
static void TestClassify(void)
{
    struct { const WCHAR *name; ProcType want; } cases[] = {
        { L"node.exe",        PT_NODE },
        { L"NODE.EXE",        PT_NODE },
        { L"nodejs.exe",      PT_NONE },
        { L"python.exe",      PT_PYTHON },
        { L"pythonW.exe",     PT_PYTHON },
        { L"python313.exe",   PT_PYTHON },
        { L"python3.13.exe",  PT_PYTHON },
        { L"pythonw313.exe",  PT_PYTHON },
        { L"pythonsetup.exe", PT_NONE },
        { L"mypython.exe",    PT_NONE },
        { L"node.txt",        PT_NONE },
    };
    int fail = 0;
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        ProcType got = ClassifyName(cases[i].name);
        if (got != cases[i].want) {
            fail++;
            wprintf(L"[FAIL] %-20ls => got %d want %d\n",
                    cases[i].name, (int)got, (int)cases[i].want);
        }
    }
    wprintf(L"classify tests: %d cases, %d failed\n",
            (int)(sizeof(cases) / sizeof(cases[0])), fail);
}

int wmain(void)
{
    ProcList l;

    TestClassify();

    ZeroMemory(&l, sizeof(l));
    int n = ScanProcesses(&l);
    wprintf(L"scan matched: %d\n", n);
    for (size_t i = 0; i < l.count; i++) {
        wprintf(L"[%ls] pid=%lu ppid=%lu mem=%lu KB name=%ls\n",
                TypeTag(l.items[i].type),
                (unsigned long)l.items[i].pid,
                (unsigned long)l.items[i].ppid,
                (unsigned long)(l.items[i].memBytes >> 10),
                l.items[i].name);
    }
    FreeProcList(&l);
    return 0;
}
