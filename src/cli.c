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
#include "config.h"
#include "klog.h"
#include "peb.h"
#include "process.h"
#include "net.h"
#include "ai.h"

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
    ProcList all;
    DWORD *pids;
    DWORD *errs;
    size_t n = 0, cap = 16;
    KillResult kr;

    /* CLI mode returns before main init; init config+log here */
    ConfigInit();
    KlogInit();
    ZeroMemory(&all, sizeof(all));
    ScanAllProcesses(&all); /* names/paths for logging; failure tolerated */

    pids = (DWORD *)malloc(cap * sizeof(DWORD));
    errs = (DWORD *)malloc(cap * sizeof(DWORD));
    if (!pids || !errs) {
        free(pids);
        free(errs);
        FreeProcList(&all);
        OutReset();
        OutRaw("{\"killed\":[]}\n");
        OutFlush();
        return 0;
    }
    while (*p) {
        while (*p == ' ' || *p == ',')
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
        if (n == cap) {
            DWORD *t;
            cap *= 2;
            t = (DWORD *)realloc(pids, cap * sizeof(DWORD));
            if (!t)
                break;
            pids = t;
            t = (DWORD *)realloc(errs, cap * sizeof(DWORD));
            if (!t)
                break;
            errs = t;
        }
        pids[n++] = (DWORD)pid;
    }

    KillPids(pids, n, &kr, errs);

    /* log every attempt (incl. failures) for traceability */
    for (size_t i = 0; i < n; i++) {
        const ProcInfo *info = NULL;
        for (size_t k = 0; k < all.count; k++)
            if (all.items[k].pid == pids[i]) {
                info = &all.items[k];
                break;
            }
        KlogWrite(L"CLI", info, pids[i], errs[i] == 0, errs[i]);
    }
    FreeProcList(&all);

    OutReset();
    OutRaw("{\"killed\":[");
    BOOL first = TRUE;
    for (size_t i = 0; i < n; i++) {
        if (errs[i] == 0) {
            if (!first)
                OutRaw(",");
            OutNum((long long)pids[i]);
            first = FALSE;
        }
    }
    OutRaw("]}\n");
    OutFlush();
    free(pids);
    free(errs);
    return 0;
}

/* ---------- 入口 ---------- */

static int CmdOrphans(const WCHAR *args)
{
    /* /orphans           全量孤儿（含系统目录护栏过滤）
     * /orphans:nodepy    仅 Node/Python 孤儿 */
    ProcList l;
    BOOL nodePyOnly = wcsstr(args, L"nodepy") != NULL;

    ZeroMemory(&l, sizeof(l));
    ScanOrphanProcesses(&l, nodePyOnly);
    OutReset();
    OutRaw("{\"orphans\":[");
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
        OutRaw("\",\"path\":");
        JsonStr(p->path);
        OutRaw("}");
    }
    OutRaw("]}\n");
    OutFlush();
    FreeProcList(&l);
    return 0;
}

/* /top [node|python|all] [N]：按内存降序输出前 N 个进程（含命令行/工作目录） */
static int CmdTop(const WCHAR *args)
{
    ProcList l;
    int want = 10;
    ProcType filter = PT_NONE; /* NONE = all */
    const WCHAR *p = args;

    if (wcsstr(p, L"python"))
        filter = PT_PYTHON;
    else if (wcsstr(p, L"all"))
        filter = PT_NONE; /* 明确 all */
    else if (wcsstr(p, L"node"))
        filter = PT_NODE;

    /* 解析尾部数字 N */
    {
        long n = 0;
        const WCHAR *d = wcsrchr(p, L' ');
        if (d)
            n = _wtol(d + 1);
        else {
            /* 也支持 "node15" 这类紧凑写法 */
            while (*p && (*p < L'0' || *p > L'9'))
                p++;
            n = _wtol(p);
        }
        if (n >= 1 && n <= 500)
            want = (int)n;
    }

    ZeroMemory(&l, sizeof(l));
    ScanAllProcesses(&l);

    /* 过滤 + 按内存降序（简单选择：先过滤到新数组再 qsort） */
    {
        int *idxArr = (int *)malloc((l.count ? l.count : 1) * sizeof(int));
        int m = 0;
        if (!idxArr) {
            FreeProcList(&l);
            OutReset();
            OutRaw("{\"top\":[]}\n");
            OutFlush();
            return 0;
        }
        for (size_t i = 0; i < l.count; i++)
            if (filter == PT_NONE || l.items[i].type == filter)
                idxArr[m++] = (int)i;
        /* 插入排序按 mem 降序（m 通常小） */
        for (int a = 1; a < m; a++) {
            int key = idxArr[a], b = a - 1;
            while (b >= 0 && l.items[idxArr[b]].memBytes < l.items[key].memBytes) {
                idxArr[b + 1] = idxArr[b];
                b--;
            }
            idxArr[b + 1] = key;
        }
        if (m > want)
            m = want;

        OutReset();
        OutRaw("{\"type\":\"");
        OutRaw(filter == PT_NODE ? "node" : (filter == PT_PYTHON ? "python" : "all"));
        OutRaw("\",\"top\":[");
        for (int i = 0; i < m; i++) {
            ProcInfo *e = &l.items[idxArr[i]];
            WCHAR cmd[1024], cwd[MAX_PATH];
            if (i)
                OutRaw(",");
            OutRaw("{\"pid\":");
            OutNum((long long)e->pid);
            OutRaw(",\"name\":");
            JsonStr(e->name);
            OutRaw(",\"ppid\":");
            OutNum((long long)e->ppid);
            OutRaw(",\"mem_kb\":");
            OutNum((long long)(e->memBytes >> 10));
            OutRaw(",\"path\":");
            JsonStr(e->path);
            if (PebQuery(e->pid, cmd, 1024, cwd, MAX_PATH)) {
                OutRaw(",\"cmdline\":");
                JsonStr(cmd);
                OutRaw(",\"cwd\":");
                JsonStr(cwd);
            } else {
                OutRaw(",\"cmdline\":\"\",\"cwd\":\"\"");
            }
            OutRaw("}");
        }
        OutRaw("]}\n");
        OutFlush();
        free(idxArr);
    }
    FreeProcList(&l);
    return 0;
}

/* ---------------- WP4: AI CLI 接口 ---------------- */

/* 同步调用 kilo（CLI 场景阻塞可接受），返回堆分配的答案或 NULL */
static WCHAR *CliKiloSync(const WCHAR *prompt)
{
    /* 简化版：直接 CreateProcess + 管道读 stdout（不走 ai.c 的线程机制） */
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR cmd[16384];
    WCHAR kiloExe[MAX_PATH];
    char raw[128 * 1024];
    DWORD rawLen = 0, got;
    ULONGLONG t0;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdInput = si.hStdError = NULL;

    /* 路径回退链（同 ai.c） */
    {
        const WCHAR *kilo = L"kilo";
        if (GetEnvironmentVariableW(L"KILO_EXE", kiloExe, MAX_PATH) > 0)
            kilo = kiloExe;
        else {
            static const WCHAR *const cands[] = {
                L"H:\\2026code\\2028-amis\\zcode-demo\\kilo-windows-x64-v7.6.2\\kilo.exe",
                L"D:\\kilo\\kilo-windows-x64\\kilo.exe",
            };
            for (int i = 0; i < 2; i++)
                if (GetFileAttributesW(cands[i]) != INVALID_FILE_ATTRIBUTES) {
                    kilo = cands[i];
                    break;
                }
        }
        StringCchPrintfW(cmd, 16384, L"\"%ls\" run \"%ls\"", kilo, prompt);
    }

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return NULL;
    }
    CloseHandle(wr);

    /* 轮询读取 + 超时（5 分钟） */
    t0 = GetTickCount64();
    while (rawLen + 1 < sizeof(raw)) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL))
            break;
        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
                /* 子进程退出，再读剩余 */
                while (ReadFile(rd, raw + rawLen, sizeof(raw) - 1 - rawLen, &got, NULL) && got)
                    rawLen += got;
                break;
            }
            if (GetTickCount64() - t0 > 300000) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            continue;
        }
        if (avail > sizeof(raw) - 1 - rawLen)
            avail = sizeof(raw) - 1 - rawLen;
        if (!ReadFile(rd, raw + rawLen, avail, &got, NULL) || !got)
            break;
        rawLen += got;
    }
    raw[rawLen] = '\0';
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);

    if (rawLen == 0)
        return NULL;
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)rawLen, NULL, 0);
        if (wlen <= 0)
            return NULL;
        {
            WCHAR *out = (WCHAR *)malloc(((size_t)wlen + 1) * sizeof(WCHAR));
            if (out) {
                MultiByteToWideChar(CP_UTF8, 0, raw, (int)rawLen, out, wlen);
                out[wlen] = L'\0';
            }
            return out;
        }
    }
}

/* /ai-query "问题" 或 /ai-query @file.txt */
static int CmdAiQuery(const WCHAR *args)
{
    WCHAR question[2048];
    WCHAR context[16384];
    WCHAR prompt[18432];
    WCHAR *answer, *json;
    ProcList l;
    NetList nl;
    int npl;

    /* 解析 @file 或直接参数 */
    while (*args == L' ')
        args++;
    if (*args == L'@') {
        DWORD n;
        HANDLE f = CreateFileW(args + 1, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            OutReset(); OutRaw("{\"error\":\"cannot read file\"}\n"); OutFlush();
            return 2;
        }
        n = GetFileSize(f, NULL);
        if (n > 4095) n = 4095;
        {
            char buf[4096] = {0};
            DWORD got;
            ReadFile(f, buf, n, &got, NULL);
            MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, question, 2048);
            question[got < 4095 ? got : 2047] = L'\0';
        }
        CloseHandle(f);
    } else {
        StringCchCopyW(question, 2048, args);
    }
    if (!question[0]) {
        OutReset(); OutRaw("{\"error\":\"empty question\"}\n"); OutFlush();
        return 1;
    }

    /* 采集上下文：进程+端口 */
    context[0] = L'\0';
    ZeroMemory(&l, sizeof(l));
    ZeroMemory(&nl, sizeof(nl));
    npl = ScanAllProcesses(&l);
    ScanListenPorts(&nl);
    {
        /* v5.3 fix: 按内存降序取前 30（优先含 node/python），而非快照顺序 */
        /* 选择排序前 30 */
        for (int rank = 0; rank < 30 && rank < npl; rank++) {
            size_t maxIdx = (size_t)rank;
            for (size_t j = rank + 1; j < l.count; j++)
                if (l.items[j].memBytes > l.items[maxIdx].memBytes)
                    maxIdx = j;
            if (maxIdx != (size_t)rank) {
                ProcInfo t = l.items[rank];
                l.items[rank] = l.items[maxIdx];
                l.items[maxIdx] = t;
            }
        }
        /* 进程摘要（Top 30 按内存） */
        StringCchCatW(context, 16384, L"Processes (top 30 by memory):\n");
        for (int i = 0; i < npl && i < 30; i++) {
            WCHAR line[512];
            StringCchPrintfW(line, 512, L"%lu\t%ls\t%luKB\t%ls\t%ls\n",
                             (unsigned long)l.items[i].pid, l.items[i].name,
                             (unsigned long)(l.items[i].memBytes >> 10),
                             l.items[i].type == PT_NODE ? L"node"
                                 : (l.items[i].type == PT_PYTHON ? L"python" : L"-"),
                             l.items[i].path);
            StringCchCatW(context, 16384, line);
        }
        /* 端口摘要（Top 30） */
        StringCchCatW(context, 16384, L"\nPorts:\n");
        for (size_t i = 0; i < nl.count && i < 30; i++) {
            WCHAR line[128];
            StringCchPrintfW(line, 128, L"%lu\t%ls\tpid=%lu\n",
                             (unsigned long)nl.items[i].port,
                             nl.items[i].tcp ? L"TCP" : L"UDP",
                             (unsigned long)nl.items[i].pid);
            StringCchCatW(context, 16384, line);
        }
        /* 保留区间 */
        {
            PortRangeList rl;
            ZeroMemory(&rl, sizeof(rl));
            ScanReservedPortRanges(&rl);
            StringCchCatW(context, 16384, L"\nReservedRanges:\n");
            for (size_t i = 0; i < rl.count; i++) {
                WCHAR line[64];
                StringCchPrintfW(line, 64, L"%lu-%lu\t%ls\n",
                                 (unsigned long)rl.items[i].start,
                                 (unsigned long)rl.items[i].end,
                                 rl.items[i].tcp ? L"TCP" : L"UDP");
                StringCchCatW(context, 16384, line);
            }
            FreePortRangeList(&rl);
        }
    }
    FreeProcList(&l);
    FreeNetList(&nl);
    AiTruncateContext(context, 12000);

    StringCchPrintfW(prompt, 18432, AiGetPrompt(AIPROMPT_AI_QUERY),
                     question, context);

    answer = CliKiloSync(prompt);
    if (!answer) {
        OutReset(); OutRaw("{\"error\":\"kilo unavailable or timeout\"}\n"); OutFlush();
        return 2;
    }
    json = AiExtractJson(answer);
    if (json) {
        /* 宽字符转 UTF-8 输出 */
        char utf8[16384];
        int n = WideCharToMultiByte(CP_UTF8, 0, json, -1, utf8, sizeof(utf8), NULL, NULL);
        OutReset();
        if (n > 1) OutRaw(utf8);
        OutRaw("\n"); OutFlush();
        free(json);
    } else {
        OutReset(); OutRaw("{\"raw\":"); JsonStr(answer); OutRaw("}\n"); OutFlush();
    }
    free(answer);
    return 0;
}

/* /ai-suggest <port> */
static int CmdAiSuggest(const WCHAR *args)
{
    DWORD port;
    WCHAR context[4096];
    WCHAR prompt[8192];
    WCHAR *answer, *json;
    NetList nl;
    PortRangeList rl;
    BOOL inReserved = FALSE;
    DWORD ownerPid = 0;

    port = (DWORD)_wtol(args);
    if (port == 0 || port > 65535) {
        OutReset(); OutRaw("{\"error\":\"invalid port\"}\n"); OutFlush();
        return 1;
    }

    /* 采集上下文 */
    context[0] = L'\0';
    ZeroMemory(&nl, sizeof(nl));
    ZeroMemory(&rl, sizeof(rl));
    ScanListenPorts(&nl);
    ScanReservedPortRanges(&rl);
    for (size_t i = 0; i < nl.count; i++)
        if (nl.items[i].port == port) {
            ownerPid = nl.items[i].pid;
            break;
        }
    for (size_t i = 0; i < rl.count; i++)
        if (port >= rl.items[i].start && port <= rl.items[i].end) {
            inReserved = TRUE;
            break;
        }
    {
        WCHAR line[256];
        if (ownerPid)
            StringCchPrintfW(line, 256, L"port=%lu LISTENING pid=%lu;", port, ownerPid);
        else
            StringCchPrintfW(line, 256, L"port=%lu NOT_LISTENING;", port);
        StringCchCatW(context, 4096, line);
        if (inReserved) {
            StringCchCatW(context, 4096,
                          L" IN_RESERVED_RANGE (winnat/Hyper-V exclusion);");
        }
        StringCchPrintfW(line, 256, L" total_reserved=%d;", (int)rl.count);
        StringCchCatW(context, 4096, line);
    }
    FreeNetList(&nl);
    FreePortRangeList(&rl);

    StringCchPrintfW(prompt, 8192, AiGetPrompt(AIPROMPT_PORT_SUGGEST),
                     (unsigned long)port, context);
    answer = CliKiloSync(prompt);
    if (!answer) {
        /* 降级：本地规则版结论（无 AI 也可用） */
        OutReset();
        if (inReserved)
            OutRaw("{\"root_cause\":\"winnat reserved port (EACCES)\",\"evidence\":\"port in exclusion range\",\"fix_steps\":[\"net stop winnat\",\"net start winnat\",\"netsh int ipv4 add excludedportrange protocol=tcp startport=N numberofports=1\"]}\n");
        else if (ownerPid)
            OutRaw("{\"root_cause\":\"port occupied by process\",\"evidence\":\"pid=N listening\",\"fix_steps\":[\"kill the process or change port\"]}\n");
        else
            OutRaw("{\"root_cause\":\"unknown\",\"evidence\":\"port not in use or reserved\",\"fix_steps\":[\"check firewall or app config\"]}\n");
        OutFlush();
        return 0;
    }
    json = AiExtractJson(answer);
    if (json) {
        /* 宽字符转 UTF-8 输出 */
        char utf8[16384];
        int un = WideCharToMultiByte(CP_UTF8, 0, json, -1, utf8, sizeof(utf8), NULL, NULL);
        OutReset();
        if (un > 1) OutRaw(utf8);
        OutRaw("\n"); OutFlush();
        free(json);
    } else {
        OutReset(); OutRaw("{\"raw\":"); JsonStr(answer); OutRaw("}\n"); OutFlush();
    }
    free(answer);
    return 0;
}

BOOL CliDispatch(const WCHAR *cmdLine)
{
    const WCHAR *cli = cmdLine ? cmdLine : L"";

    if (wcsstr(cli, L"/list") == NULL &&
        wcsstr(cli, L"/ports") == NULL &&
        wcsstr(cli, L"/kill") == NULL &&
        wcsstr(cli, L"/orphans") == NULL &&
        wcsstr(cli, L"/top") == NULL &&
        wcsstr(cli, L"/ai-query") == NULL &&
        wcsstr(cli, L"/ai-suggest") == NULL)
        return FALSE;

    if (!InitOutput())
        return TRUE;

    if (wcsstr(cli, L"/ports"))
        CmdPorts();
    else if (wcsstr(cli, L"/kill"))
        CmdKill(wcsstr(cli, L"/kill") + 5);
    else if (wcsstr(cli, L"/orphans"))
        CmdOrphans(wcsstr(cli, L"/orphans") + 9);
    else if (wcsstr(cli, L"/top"))
        CmdTop(wcsstr(cli, L"/top") + 4);
    else if (wcsstr(cli, L"/ai-query"))
        CmdAiQuery(wcsstr(cli, L"/ai-query") + 10);
    else if (wcsstr(cli, L"/ai-suggest"))
        CmdAiSuggest(wcsstr(cli, L"/ai-suggest") + 11);
    else
        CmdList();

    OutFlush();
    FreeConsole(); /* AttachConsole 场景下安全分离 */
    return TRUE;
}
