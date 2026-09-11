/* cli.c - 无头 CLI 实现：输出 UTF-8 JSON 到 stdout
 * 用法：
 *   kill-process-type.exe /list            全部进程（含类型标注）
 *   kill-process-type.exe /ports           端口监听 + 系统保留区间
 *   kill-process-type.exe /kill 1234 5678  按 PID 终止
 * 退出码：0 成功 / 1 参数错误 / 2 无输出句柄
 */
#include "common.h"
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "cli.h"
#include "process.h"
#include "net.h"

/* ---------- 输出 ---------- */

static HANDLE s_out;
static char s_buf[1 << 16];
static size_t s_len;

static void OutReset(void) { s_len = 0; }

static void OutFlush(void)
{
    DWORD written = 0;

    if (!s_out)
        return;
    if (s_len) {
        WriteFile(s_out, s_buf, (DWORD)s_len, &written, NULL);
        s_len = 0;
    }
}

static void OutRaw(const char *s)
{
    while (*s) {
        if (s_len + 1 >= sizeof(s_buf))
            OutFlush();
        s_buf[s_len++] = *s++;
    }
}

/* 宽字符转 UTF-8 逐字符输出 */
static void OutW1(WCHAR c)
{
    char tmp[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, tmp, sizeof(tmp), NULL, NULL);

    if (n > 0)
        for (int i = 0; i < n; i++) {
            if (s_len + 1 >= sizeof(s_buf))
                OutFlush();
            s_buf[s_len++] = tmp[i];
        }
}

/* JSON 字符串（重写为逐字符版本，简单可靠） */
static void JsonStr(const WCHAR *s)
{
    OutRaw("\"");
    for (; s && *s; s++) {
        switch (*s) {
        case L'"':  OutRaw("\\\""); break;
        case L'\\': OutRaw("\\\\"); break;
        case L'\n': OutRaw("\\n");  break;
        case L'\r': break;
        case L'\t': OutRaw("\\t");  break;
        default:
            if (*s < 0x20) {
                char esc[8];
                StringCchPrintfA(esc, 8, "\\u%04x", (unsigned)*s);
                OutRaw(esc);
            } else {
                OutW1(*s);
            }
            break;
        }
    }
    OutRaw("\"");
}

static void OutNum(long long v)
{
    char tmp[32];
    StringCchPrintfA(tmp, 32, "%lld", v);
    OutRaw(tmp);
}

/* ---------- 获取输出句柄（支持重定向管道与交互控制台） ---------- */

static BOOL InitOutput(void)
{
    s_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (s_out && s_out != INVALID_HANDLE_VALUE)
        return TRUE;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        s_out = GetStdHandle(STD_OUTPUT_HANDLE);
        return s_out && s_out != INVALID_HANDLE_VALUE;
    }
    return FALSE;
}

/* ---------- 子命令 ---------- */

static const char *TypeTag(ProcType t)
{
    switch (t) {
    case PT_NODE:   return "node";
    case PT_PYTHON: return "python";
    default:        return "other";
    }
}

static int CmdList(void)
{
    ProcList l;
    int n;

    ZeroMemory(&l, sizeof(l));
    n = ScanAllProcesses(&l);
    OutReset();
    OutRaw("{\"count\":");
    OutNum(n < 0 ? 0 : n);
    OutRaw(",\"processes\":[");
    for (size_t i = 0; i < l.count; i++) {
        ProcInfo *p = &l.items[i];
        if (i)
            OutRaw(",");
        OutRaw("{\"pid\":");
        OutNum((long long)p->pid);
        OutRaw(",\"ppid\":");
        OutNum((long long)p->ppid);
        OutRaw(",\"name\":");
        JsonStr(p->name);
        OutRaw(",\"type\":\"");
        OutRaw(TypeTag(p->type));
        OutRaw("\",\"mem_kb\":");
        OutNum((long long)(p->memBytes >> 10));
        OutRaw(",\"path\":");
        JsonStr(p->path);
        OutRaw("}");
    }
    OutRaw("]}\n");
    OutFlush();
    FreeProcList(&l);
    return 0;
}

static int CmdPorts(void)
{
    NetList nl;
    PortRangeList rl;

    ZeroMemory(&nl, sizeof(nl));
    ZeroMemory(&rl, sizeof(rl));
    ScanListenPorts(&nl);
    ScanReservedPortRanges(&rl);

    OutReset();
    OutRaw("{\"listeners\":[");
    for (size_t i = 0; i < nl.count; i++) {
        NetEntry *e = &nl.items[i];
        if (i)
            OutRaw(",");
        OutRaw("{\"port\":");
        OutNum((long long)e->port);
        OutRaw(",\"proto\":\"");
        OutRaw(e->tcp ? (e->ipv6 ? "tcp6" : "tcp") : (e->ipv6 ? "udp6" : "udp"));
        OutRaw("\",\"pid\":");
        OutNum((long long)e->pid);
        OutRaw("}");
    }
    OutRaw("],\"reserved\":[");
    for (size_t i = 0; i < rl.count; i++) {
        PortRange *r = &rl.items[i];
        if (i)
            OutRaw(",");
        OutRaw("{\"start\":");
        OutNum((long long)r->start);
        OutRaw(",\"end\":");
        OutNum((long long)r->end);
        OutRaw(",\"proto\":\"");
        OutRaw(r->tcp ? (r->ipv6 ? "tcp6" : "tcp") : (r->ipv6 ? "udp6" : "udp"));
        OutRaw("\"}");
    }
    OutRaw("]}\n");
    OutFlush();
    FreeNetList(&nl);
    FreePortRangeList(&rl);
    return 0;
}

static int CmdKill(const WCHAR *args)
{
    const WCHAR *p = args;

    OutReset();
    OutRaw("{\"killed\":[");
    BOOL first = TRUE;
    while (*p) {
        while (*p == L' ' || *p == L',')
            p++;
        if (!*p)
            break;
        long pid = 0;
        int any = 0;
        while (*p >= L'0' && *p <= L'9') {
            pid = pid * 10 + (*p - L'0');
            p++;
            any = 1;
        }
        if (!any)
            break;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (h && TerminateProcess(h, 1)) {
            if (!first)
                OutRaw(",");
            OutNum(pid);
            first = FALSE;
        }
        if (h)
            CloseHandle(h);
    }
    OutRaw("]}\n");
    OutFlush();
    return 0;
}

/* ---------- 入口 ---------- */

BOOL CliDispatch(const WCHAR *cmdLine)
{
    const WCHAR *cli = cmdLine ? cmdLine : L"";

    if (wcsstr(cli, L"/list") == NULL &&
        wcsstr(cli, L"/ports") == NULL &&
        wcsstr(cli, L"/kill") == NULL)
        return FALSE;

    if (!InitOutput())
        return TRUE; /* 无输出通道（如双击启动）也不进 GUI，直接退出 */

    if (wcsstr(cli, L"/ports"))
        CmdPorts();
    else if (wcsstr(cli, L"/kill"))
        CmdKill(wcsstr(cli, L"/kill") + 5);
    else
        CmdList();

    OutFlush();
    FreeConsole(); /* AttachConsole 场景下安全分离 */
    return TRUE;
}
