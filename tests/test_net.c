/* test_net.c - net 模块验证：打印双栈监听端口表（只读，不杀进程）
 * 编译: gcc -municode -O2 -Wall -static -o build\test_net.exe tests\test_net.c src\net.c -liphlpapi
 */
#include "../src/common.h"
#include <stdio.h>
#include "../src/net.h"

int wmain(void)
{
    NetList l;
    PortRangeList rl;
    int prevPort = -1;
    int badOrder = 0;

    ZeroMemory(&l, sizeof(l));
    int n = ScanListenPorts(&l);
    wprintf(L"listen entries: %d\n", n);
    for (size_t i = 0; i < l.count; i++) {
        const NetEntry *e = &l.items[i];
        if ((int)e->port < prevPort)
            badOrder++;
        prevPort = (int)e->port;
        wprintf(L"%-6lu %ls  pid=%lu\n",
                (unsigned long)e->port,
                e->tcp ? (e->ipv6 ? L"TCP6" : L"TCP")
                       : (e->ipv6 ? L"UDP6" : L"UDP"),
                (unsigned long)e->pid);
    }
    wprintf(L"order check: %d violations\n", badOrder);
    FreeNetList(&l);

    ZeroMemory(&rl, sizeof(rl));
    int rn = ScanReservedPortRanges(&rl);
    wprintf(L"reserved port ranges: %d\n", rn);
    for (size_t i = 0; i < rl.count; i++)
        wprintf(L"%lu-%lu %ls\n",
                (unsigned long)rl.items[i].start,
                (unsigned long)rl.items[i].end,
                rl.items[i].tcp ? (rl.items[i].ipv6 ? L"TCP6" : L"TCP")
                                : (rl.items[i].ipv6 ? L"UDP6" : L"UDP"));
    FreePortRangeList(&rl);
    return 0;
}
