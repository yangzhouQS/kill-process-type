/* net.c - 端口监听表扫描：IPv4/IPv6 双栈 TCP LISTEN + UDP（GetExtendedTcpTable/UdpTable）
 * + 系统保留端口区间解析（netsh excludedportrange，定位 winnat 保留导致的 EACCES）
 */
#include "common.h"
#include <strsafe.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <stdlib.h>

#include "net.h"

/* 表中的 dwLocalPort 为网络字节序，转为主机字节序 */
static DWORD PortToHost(DWORD netPort)
{
    return ((netPort & 0xff) << 8) | ((netPort >> 8) & 0xff);
}

static BOOL PushNet(NetList *l, DWORD pid, DWORD port, BOOL tcp, BOOL ipv6)
{
    if (l->count == l->cap) {
        size_t newCap = l->cap ? l->cap * 2 : 128;
        NetEntry *tmp = (NetEntry *)realloc(l->items, newCap * sizeof(NetEntry));
        if (!tmp)
            return FALSE;
        l->items = tmp;
        l->cap = newCap;
    }
    l->items[l->count].pid = pid;
    l->items[l->count].port = port;
    l->items[l->count].tcp = tcp;
    l->items[l->count].ipv6 = ipv6;
    l->count++;
    return TRUE;
}

static int CmpNet(const void *a, const void *b)
{
    const NetEntry *x = (const NetEntry *)a;
    const NetEntry *y = (const NetEntry *)b;

    if (x->port != y->port)
        return x->port < y->port ? -1 : 1;
    if (x->tcp != y->tcp)
        return x->tcp ? -1 : 1;   /* 同端口 TCP 在前 */
    if (x->ipv6 != y->ipv6)
        return x->ipv6 ? 1 : -1;  /* IPv4 在前 */
    return 0;
}

/* 通用两段式取表：先查大小再取数据，逐行写入列表 */
static BOOL LoadTcp4(NetList *out)
{
    ULONG size = 0;
    MIB_TCPTABLE_OWNER_PID *t;
    DWORD i;

    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return TRUE; /* 空表属正常 */
    t = (MIB_TCPTABLE_OWNER_PID *)malloc(size);
    if (!t)
        return FALSE;
    if (GetExtendedTcpTable(t, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR)
        for (i = 0; i < t->dwNumEntries; i++)
            if (!PushNet(out, t->table[i].dwOwningPid,
                         PortToHost(t->table[i].dwLocalPort), TRUE, FALSE))
                break;
    free(t);
    return TRUE;
}

static BOOL LoadTcp6(NetList *out)
{
    ULONG size = 0;
    MIB_TCP6TABLE_OWNER_PID *t;
    DWORD i;

    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET6,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return TRUE;
    t = (MIB_TCP6TABLE_OWNER_PID *)malloc(size);
    if (!t)
        return FALSE;
    if (GetExtendedTcpTable(t, &size, FALSE, AF_INET6,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR)
        for (i = 0; i < t->dwNumEntries; i++)
            if (!PushNet(out, t->table[i].dwOwningPid,
                         PortToHost(t->table[i].dwLocalPort), TRUE, TRUE))
                break;
    free(t);
    return TRUE;
}

static BOOL LoadUdp4(NetList *out)
{
    ULONG size = 0;
    MIB_UDPTABLE_OWNER_PID *t;
    DWORD i;

    if (GetExtendedUdpTable(NULL, &size, FALSE, AF_INET,
                            UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER)
        return TRUE;
    t = (MIB_UDPTABLE_OWNER_PID *)malloc(size);
    if (!t)
        return FALSE;
    if (GetExtendedUdpTable(t, &size, FALSE, AF_INET,
                            UDP_TABLE_OWNER_PID, 0) == NO_ERROR)
        for (i = 0; i < t->dwNumEntries; i++)
            if (!PushNet(out, t->table[i].dwOwningPid,
                         PortToHost(t->table[i].dwLocalPort), FALSE, FALSE))
                break;
    free(t);
    return TRUE;
}

static BOOL LoadUdp6(NetList *out)
{
    ULONG size = 0;
    MIB_UDP6TABLE_OWNER_PID *t;
    DWORD i;

    if (GetExtendedUdpTable(NULL, &size, FALSE, AF_INET6,
                            UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER)
        return TRUE;
    t = (MIB_UDP6TABLE_OWNER_PID *)malloc(size);
    if (!t)
        return FALSE;
    if (GetExtendedUdpTable(t, &size, FALSE, AF_INET6,
                            UDP_TABLE_OWNER_PID, 0) == NO_ERROR)
        for (i = 0; i < t->dwNumEntries; i++)
            if (!PushNet(out, t->table[i].dwOwningPid,
                         PortToHost(t->table[i].dwLocalPort), FALSE, TRUE))
                break;
    free(t);
    return TRUE;
}

int ScanListenPorts(NetList *out)
{
    ZeroMemory(out, sizeof(*out));

    if (!LoadTcp4(out) || !LoadTcp6(out) || !LoadUdp4(out) || !LoadUdp6(out)) {
        FreeNetList(out);
        return -1;
    }
    if (out->count > 1)
        qsort(out->items, out->count, sizeof(NetEntry), CmpNet);
    return (int)out->count;
}

void FreeNetList(NetList *l)
{
    if (!l)
        return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

/* ---------------- 系统保留端口区间 ---------------- */

static BOOL PushRange(PortRangeList *l, WORD start, WORD end, BOOL tcp, BOOL ipv6)
{
    if (l->count == l->cap) {
        size_t newCap = l->cap ? l->cap * 2 : 32;
        PortRange *tmp = (PortRange *)realloc(l->items, newCap * sizeof(PortRange));
        if (!tmp)
            return FALSE;
        l->items = tmp;
        l->cap = newCap;
    }
    l->items[l->count].start = start;
    l->items[l->count].end = end;
    l->items[l->count].tcp = tcp;
    l->items[l->count].ipv6 = ipv6;
    l->count++;
    return TRUE;
}

/* 静默运行 netsh 并捕获输出（CREATE_NO_WINDOW，无控制台闪窗） */
static BOOL RunNetshCapture(const WCHAR *args, char *buf, DWORD cap, DWORD *outLen)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR cmd[256];
    DWORD total = 0, got;
    BOOL ok = FALSE;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return FALSE;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;

    StringCchPrintfW(cmd, 256, L"netsh %ls", args);
    if (CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(wr);
        wr = NULL;
        while (total + 1 < cap && ReadFile(rd, buf + total, cap - 1 - total, &got, NULL) && got)
            total += got;
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ok = TRUE;
    }
    if (wr)
        CloseHandle(wr);
    CloseHandle(rd);
    buf[total] = '\0';
    if (outLen)
        *outLen = total;
    return ok;
}

/* 逐行解析：仅含数字与空白的行，取前两个数字为 (起始端口, 结束端口)。
 * 该规则与输出语言无关（中文/英文表头均无 ASCII 数字）。 */
static void ParseRangeLines(const char *text, BOOL tcp, BOOL ipv6, PortRangeList *out)
{
    const char *p = text;

    while (p && *p) {
        const char *eol = p;
        const char *q;
        long nums[2] = { -1, -1 };
        int nnums = 0, onlyNumWs = 1;

        while (*eol && *eol != '\n')
            eol++;
        q = p;
        while (q < eol) {
            char c = *q;
            if (c >= '0' && c <= '9') {
                long v = 0;
                int len = 0;
                while (q < eol && *q >= '0' && *q <= '9') {
                    v = v * 10 + (*q - '0');
                    q++;
                    len++;
                }
                if (len > 5)
                    onlyNumWs = 0; /* 异常长数字，视作非数据行 */
                if (nnums < 2)
                    nums[nnums] = v;
                nnums++;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                q++;
            } else {
                onlyNumWs = 0; /* 表头/说明等含文字的行 */
                q++;
            }
        }
        if (onlyNumWs && nnums >= 2 &&
            nums[0] >= 1 && nums[1] >= nums[0] && nums[1] <= 65535)
            PushRange(out, (WORD)nums[0], (WORD)nums[1], tcp, ipv6);
        p = *eol ? eol + 1 : eol;
    }
}

int ScanReservedPortRanges(PortRangeList *out)
{
    static const struct { const WCHAR *args; BOOL tcp; BOOL ipv6; } kQueries[] = {
        { L"int ipv4 show excludedportrange protocol=tcp", TRUE,  FALSE },
        { L"int ipv4 show excludedportrange protocol=udp", FALSE, FALSE },
        { L"int ipv6 show excludedportrange protocol=tcp", TRUE,  TRUE  },
        { L"int ipv6 show excludedportrange protocol=udp", FALSE, TRUE  },
    };
    char buf[8192];

    ZeroMemory(out, sizeof(*out));
    for (int i = 0; i < 4; i++) {
        DWORD len = 0;
        if (!RunNetshCapture(kQueries[i].args, buf, sizeof(buf), &len))
            continue; /* netsh 不可用时尽力而为 */
        if (len)
            ParseRangeLines(buf, kQueries[i].tcp, kQueries[i].ipv6, out);
    }
    return (int)out->count;
}

void FreePortRangeList(PortRangeList *l)
{
    if (!l)
        return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}
