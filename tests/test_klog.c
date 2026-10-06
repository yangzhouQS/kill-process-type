/* test_klog.c - 用真实日志文件压测 KlogLoad/KlogFree（复现堆损坏）
 * gcc -municode -O2 -g -o build\test_klog.exe tests\test_klog.c src\klog.c src\config.c
 */
#include "../src/common.h"
#include <stdio.h>
#include "../src/config.h"
#include "../src/klog.h"

int wmain(int argc, wchar_t **argv)
{
    LogList l;
    int totalOk = 0, totalSkip = 0;

    if (argc < 2) {
        wprintf(L"usage: test_klog <logfile> [rounds]\n");
        return 1;
    }
    ConfigInit(); /* klog 依赖 config 路径……直接给 KlogInit 前手动设？ */
    /* KlogInit 从 config 派生路径；这里直接用参数文件：临时把 config 指向日志所在目录 */
    {
        /* 简化：把 exe 的 ini 路径机制绕过 —— 直接调用内部不可行，
           改为将日志复制到 config 路径再测 */
        const WCHAR *cfg = ConfigGetPath();
        wprintf(L"config path: %ls\n", cfg);
    }
    KlogInit();
    wprintf(L"log path: %ls\n", KlogGetPath());

    int rounds = (argc >= 3) ? _wtoi(argv[1 + 0 + 1 - 1]) : 200; /* argv[2] */
    if (rounds <= 0)
        rounds = 200;

    for (int r = 0; r < rounds; r++) {
        ZeroMemory(&l, sizeof(l));
        int n = KlogLoad(&l);
        if (n < 0) {
            wprintf(L"round %d: KlogLoad FAILED\n", r);
            return 2;
        }
        totalOk += n;
        /* 校验每条记录的 NUL 终止性（越界写检测） */
        for (int i = 0; i < n; i++) {
            LogEntry *e = &l.items[i];
            if (e->timeText[23] != 0 || e->source[15] != 0 || e->name[63] != 0 ||
                e->path[MAX_PATH - 1] != 0) {
                wprintf(L"round %d entry %d: NUL-termination violated! timeText[23]=%u source[15]=%u name[63]=%u path[%d]=%u\n",
                        r, i, (unsigned)e->timeText[23], (unsigned)e->source[15],
                        (unsigned)e->name[63], MAX_PATH - 1,
                        (unsigned)e->path[MAX_PATH - 1]);
                return 3;
            }
        }
        KlogFree(&l);
        totalSkip++;
    }
    wprintf(L"OK: %d rounds, %d entries total (avg %.1f)\n",
            rounds, totalOk, (double)totalOk / (rounds ? rounds : 1));
    return 0;
}
