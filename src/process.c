/* process.c - 进程枚举与终止实现（Toolhelp32 快照 + TerminateProcess） */
#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#include "process.h"
#include "peb.h"
#include "project.h"

const WCHAR *ProcTypeLabel(ProcType t)
{
    switch (t) {
    case PT_NODE:   return L"Node.js";
    case PT_PYTHON: return L"Python";
    default:        return L"-";
    }
}

/* 仅按主进程名匹配（不含路径），匹配规则：
 *   Node:   node.exe
 *   Python: python.exe / pythonw.exe / python313.exe / python3.13.exe / pythonw313.exe 等
 */
ProcType ClassifyName(const WCHAR *exeName)
{
    WCHAR low[MAX_PATH];
    size_t i = 0;

    for (; exeName[i] && i + 1 < MAX_PATH; i++)
        low[i] = (WCHAR)towlower((wint_t)exeName[i]);
    low[i] = 0;

    size_t len = i;
    if (len < 5 || len > 44)
        return PT_NONE;
    if (wcscmp(low + len - 4, L".exe") != 0)
        return PT_NONE;
    low[len - 4] = L'\0'; /* 去掉 .exe 后缀 */

    if (wcscmp(low, L"node") == 0)
        return PT_NODE;

    if (wcsncmp(low, L"python", 6) == 0) {
        size_t k = 6;
        if (low[k] == L'w')
            k++;
        for (; low[k]; k++) {
            WCHAR c = low[k];
            if (!((c >= L'0' && c <= L'9') || c == L'.'))
                return PT_NONE;
        }
        return PT_PYTHON;
    }
    return PT_NONE;
}

void FreeProcList(ProcList *l)
{
    if (!l)
        return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static int PushProc(ProcList *l, const ProcInfo *pi)
{
    if (l->count == l->cap) {
        size_t newCap = l->cap ? l->cap * 2 : 64;
        ProcInfo *tmp = (ProcInfo *)realloc(l->items, newCap * sizeof(ProcInfo));
        if (!tmp)
            return 0;
        l->items = tmp;
        l->cap = newCap;
    }
    l->items[l->count++] = *pi;
    return 1;
}

/* targetOnly=TRUE 仅收 node/python，FALSE 收全部进程 */
static int CollectSnapshot(ProcList *out, BOOL targetOnly)
{
    ZeroMemory(out, sizeof(*out));

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    BOOL ok = Process32FirstW(snap, &pe);
    while (ok) {
        ProcType t = ClassifyName(pe.szExeFile);
        if (!targetOnly || t != PT_NONE) {
            ProcInfo pi;
            ZeroMemory(&pi, sizeof(pi));
            pi.type = t;
            pi.pid = pe.th32ProcessID;
            pi.ppid = pe.th32ParentProcessID;
            StringCchCopyW(pi.name, 64, pe.szExeFile);

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.pid);
            if (h) {
                DWORD sz = MAX_PATH;
                if (!QueryFullProcessImageNameW(h, 0, pi.path, &sz))
                    pi.path[0] = L'\0';
                PROCESS_MEMORY_COUNTERS pmc;
                ZeroMemory(&pmc, sizeof(pmc));
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
                    pi.memBytes = (unsigned long long)pmc.WorkingSetSize;
                CloseHandle(h);
            }
            /* 命令行与项目归属仅对 node/python 采集（dev 语义相关） */
            if (t != PT_NONE) {
                PebQuery(pi.pid, pi.cmdline, 208, NULL, 0);
                GetProcessProject(pi.pid, pi.project, MAX_PATH);
            }
            if (!PushProc(out, &pi))
                break; /* 内存不足：提前结束枚举，返回已收集结果 */
        }
        ok = Process32NextW(snap, &pe);
    }

    CloseHandle(snap);
    return (int)out->count;
}

int ScanProcesses(ProcList *out)
{
    return CollectSnapshot(out, TRUE);
}

int ScanAllProcesses(ProcList *out)
{
    return CollectSnapshot(out, FALSE);
}

/* ---------------- 孤儿进程扫描 ---------------- */

/* 判断 pid 是否存在于快照（存活集合）。O(n²) 对几百进程完全够用 */
static BOOL IsPidAlive(const ProcList *l, DWORD pid)
{
    for (size_t i = 0; i < l->count; i++)
        if (l->items[i].pid == pid)
            return TRUE;
    return FALSE;
}

/* 路径位于系统 Windows 目录下（如 svchost/系统组件），全量模式的安全护栏 */
static BOOL IsUnderWindowsDir(const WCHAR *path)
{
    WCHAR winDir[MAX_PATH];
    DWORD n;

    if (!path || !path[0])
        return FALSE;
    n = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return FALSE;
    /* 比较目录前缀（含尾部反斜杠），不区分大小写 */
    {
        size_t wl = lstrlenW(winDir);
        if (winDir[wl - 1] != L'\\') {
            winDir[wl] = L'\\';
            winDir[wl + 1] = L'\0';
            wl++;
        }
        if ((size_t)lstrlenW(path) <= wl)
            return FALSE;
        return _wcsnicmp(path, winDir, wl) == 0;
    }
}

int ScanOrphanProcesses(ProcList *out, BOOL nodePythonOnly)
{
    ProcList all;
    DWORD selfPid = GetCurrentProcessId();

    ZeroMemory(out, sizeof(*out));
    ZeroMemory(&all, sizeof(all));
    if (ScanAllProcesses(&all) < 0)
        return -1;

    for (size_t i = 0; i < all.count; i++) {
        ProcInfo *p = &all.items[i];

        if (p->pid == 0 || p->pid == 4)
            continue;               /* 系统内核对象 */
        if (p->pid == selfPid)
            continue;               /* 本进程 */
        if (p->ppid == 0)
            continue;               /* 无父进程语义（由内核/会话管理器直接派生） */
        if (!p->path[0])
            continue; /* 路径不可读（SYSTEM 级会话进程如 csrss/winlogon，
                          ppid 天然已退出）：无法判断归属，一律不动 */
        if (nodePythonOnly && p->type == PT_NONE)
            continue;
        if (!nodePythonOnly && IsUnderWindowsDir(p->path))
            continue;               /* 全量模式：跳过系统目录进程 */
        if (IsPidAlive(&all, p->ppid))
            continue;               /* 父进程仍在，非孤儿 */
        PushProc(out, p);
    }

    FreeProcList(&all);
    return (int)out->count;
}

/* 追加一条失败明细；缓冲区将满时写入省略提示并停止追加，不静默截断 */
static void FailDetailAppend(KillResult *res, const WCHAR *line)
{
    const WCHAR *note = L"\r\n(失败明细过多，其余已省略)";
    size_t cur = 0, len = 0;

    if (res->truncated)
        return;
    if (FAILED(StringCchLengthW(res->failDetail, 1024, &cur)))
        return;
    if (FAILED(StringCchLengthW(line, 96, &len)))
        return;
    if (cur + len + 1 >= 1024 - 32) { /* 尾部预留省略提示空间 */
        StringCchCatW(res->failDetail, 1024, note);
        res->truncated = TRUE;
        return;
    }
    StringCchCatW(res->failDetail, 1024, line);
}

void KillPids(const DWORD *pids, size_t count, KillResult *res, DWORD *errOut)
{
    res->okCount = 0;
    res->failCount = 0;
    res->truncated = FALSE;
    res->failDetail[0] = L'\0';

    for (size_t i = 0; i < count; i++) {
        DWORD pid = pids[i];
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) {
            DWORD e = GetLastError();
            res->failCount++;
            if (errOut)
                errOut[i] = e;
            WCHAR line[96];
            StringCchPrintfW(line, 96, L"PID %lu：打开失败（错误 %lu）\r\n",
                             (unsigned long)pid, (unsigned long)e);
            FailDetailAppend(res, line);
            continue;
        }
        if (TerminateProcess(h, 1)) {
            res->okCount++;
            if (errOut)
                errOut[i] = 0;
        } else {
            DWORD e = GetLastError();
            res->failCount++;
            if (errOut)
                errOut[i] = e;
            WCHAR line[96];
            StringCchPrintfW(line, 96, L"PID %lu：终止失败（错误 %lu）\r\n",
                             (unsigned long)pid, (unsigned long)e);
            FailDetailAppend(res, line);
        }
        CloseHandle(h);
    }
}
