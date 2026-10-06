/* views.c - 视图渲染实现：三种视图（全部进程 / Node-Python / 端口占用）
 * 职责：列定义、筛选匹配、快照→行填充、勾选保持、状态栏统计。
 * 不做任何终止/复制等用户动作（见 actions.c）。
 */
#include "common.h"
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#include "app.h"
#include "klog.h"
#include "net.h"
#include "resource.h"
#include "views.h"

/* ---------------- 列定义 ---------------- */

typedef struct { const WCHAR *text; int width; } ColDef;

/* 全部进程与 Node/Python 视图共用列布局 */
static const ColDef kColsProc[] = {
    { L"进程名",     170 },
    { L"PID",         70 },
    { L"父PID",       70 },
    { L"内存",        90 },
    { L"类型",        80 },
    { L"可执行路径", 320 },
    { L"AI风险",      70 },
};
static const ColDef kColsProcTree[] = {
    { L"进程名（树形）", 300 },
    { L"PID",             70 },
    { L"父PID",           70 },
    { L"内存",            90 },
    { L"类型",            80 },
    { L"可执行路径",     380 },
};
static const ColDef kColsProcCmd[] = {
    { L"进程名",     170 },
    { L"PID",         70 },
    { L"父PID",       70 },
    { L"内存",        90 },
    { L"类型",        80 },
    { L"可执行路径", 280 },
    { L"命令行",     360 },
    { L"AI风险",      70 },
};
static const ColDef kColsPort[] = {
    { L"端口",        80 },
    { L"协议",        80 },
    { L"PID",         80 },
    { L"进程名",     170 },
    { L"类型",        80 },
    { L"内存",        90 },
    { L"可执行路径", 420 },
};
static const ColDef kColsLog[] = {
    { L"时间",       130 },
    { L"来源",       100 },
    { L"进程名",     150 },
    { L"PID",         80 },
    { L"结果",        80 },
    { L"可执行路径", 460 },
};

void ViewsSetColumns(void)
{
    const ColDef *cols;
    int nCols, i;
    LVCOLUMNW lvc;

    if (!g_app.hList)
        return;
    if (g_app.mode == MODE_DIAG) {
        /* WP5: 诊断页签无列表列 */
        while (ListView_DeleteColumn(g_app.hList, 0))
            ;
        return;
    }
    while (ListView_DeleteColumn(g_app.hList, 0))
        ;

    if (g_app.mode == MODE_PORT) {
        cols = kColsPort;
        nCols = (int)(sizeof(kColsPort) / sizeof(kColsPort[0]));
    } else if (g_app.mode == MODE_LOG) {
        cols = kColsLog;
        nCols = (int)(sizeof(kColsLog) / sizeof(kColsLog[0]));
    } else if (g_app.mode == MODE_ALL && g_app.treeMode) {
        cols = kColsProcTree;
        nCols = (int)(sizeof(kColsProcTree) / sizeof(kColsProcTree[0]));
    } else if (g_app.mode == MODE_PROC) {
        cols = kColsProcCmd; /* Node/Python 视图带命令行列 */
        nCols = (int)(sizeof(kColsProcCmd) / sizeof(kColsProcCmd[0]));
    } else {
        cols = kColsProc;
        nCols = (int)(sizeof(kColsProc) / sizeof(kColsProc[0]));
    }
    ZeroMemory(&lvc, sizeof(lvc));
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (i = 0; i < nCols; i++) {
        lvc.iSubItem = i;
        lvc.cx = AppScale(cols[i].width);
        lvc.pszText = (LPWSTR)cols[i].text;
        ListView_InsertColumn(g_app.hList, i, &lvc);
    }
}

/* ---------------- 筛选 ---------------- */

static void GetFilterText(WCHAR *buf, size_t cch)
{
    if (g_app.hEditFilter && buf && cch)
        GetWindowTextW(g_app.hEditFilter, buf, (int)cch);
    else if (buf)
        buf[0] = L'\0';
}

static void ToLowerBuf(WCHAR *s)
{
    for (; *s; s++)
        *s = (WCHAR)towlower((wint_t)*s);
}

/* 进程名子串筛选（不区分大小写），空筛选匹配全部 */
static BOOL NameMatch(const WCHAR *name, const WCHAR *filter)
{
    WCHAR ln[64], lf[64];

    if (!filter[0])
        return TRUE;
    StringCchCopyW(ln, 64, name ? name : L"");
    StringCchCopyW(lf, 64, filter);
    ToLowerBuf(ln);
    ToLowerBuf(lf);
    return wcsstr(ln, lf) != NULL;
}

/* 端口筛选：支持 "3000" / "80,443" / "3000-3010" 组合，空筛选匹配全部 */
static BOOL PortMatch(DWORD port, const WCHAR *filter)
{
    const WCHAR *p = filter;

    if (!filter[0])
        return TRUE;
    while (*p) {
        WCHAR *end = NULL;
        unsigned long a, b;

        while (*p == L' ' || *p == L',')
            p++;
        if (!*p)
            break;
        a = wcstoul(p, &end, 10);
        if (end == p) { /* 跳过无法解析的段 */
            while (*p && *p != L',')
                p++;
            continue;
        }
        p = end;
        b = a;
        if (*p == L'-' || *p == L'~') {
            unsigned long b2 = wcstoul(p + 1, &end, 10);
            if (end != p + 1) {
                b = b2;
                p = end;
            }
        }
        if (a > b) {
            unsigned long t = a;
            a = b;
            b = t;
        }
        if (port >= a && port <= b)
            return TRUE;
    }
    return FALSE;
}

/* 保留区间行的筛选匹配：filter 端口段与 [start,end] 相交即命中；
 * 超大区间只测端点与中点，避免逐端口遍历 */
static BOOL PortRangeMatch(WORD start, WORD end, const WCHAR *filter)
{
    DWORD p;

    if (!filter[0])
        return TRUE;
    if ((DWORD)end - start > 4096)
        return PortMatch(start, filter) || PortMatch(end, filter) ||
               PortMatch(start + ((DWORD)end - start) / 2, filter);
    for (p = start; p <= (DWORD)end; p++)
        if (PortMatch(p, filter))
            return TRUE;
    return FALSE;
}

/* ---------------- 勾选保持（跨刷新） ---------------- */

/* 保存列表当前勾选项对应的 PID，刷新后据此回放，避免自动刷新丢失勾选 */
static void SaveCheckedPids(DWORD **outPids, size_t *outCount)
{
    int cnt = g_app.hList ? ListView_GetItemCount(g_app.hList) : 0;
    DWORD *pids = NULL;
    size_t n = 0;

    if (cnt > 0) {
        pids = (DWORD *)malloc((size_t)cnt * sizeof(DWORD));
        if (pids) {
            for (int i = 0; i < cnt; i++) {
                LVITEMW it;
                if (!ListView_GetCheckState(g_app.hList, i))
                    continue;
                ZeroMemory(&it, sizeof(it));
                it.mask = LVIF_PARAM;
                it.iItem = i;
                if (ListView_GetItem(g_app.hList, &it))
                    pids[n++] = (DWORD)it.lParam;
            }
        }
    }
    *outPids = pids;
    *outCount = n;
}

/* 按 PID 回放勾选；刷新间隔内已退出的进程自然丢弃 */
static void RestoreCheckedPids(const DWORD *pids, size_t count)
{
    int cnt;

    if (!pids || !count || !g_app.hList)
        return;
    cnt = ListView_GetItemCount(g_app.hList);
    for (int i = 0; i < cnt; i++) {
        LVITEMW it;
        DWORD pid;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = i;
        if (!ListView_GetItem(g_app.hList, &it))
            continue;
        pid = (DWORD)it.lParam;
        for (size_t k = 0; k < count; k++) {
            if (pids[k] == pid) {
                ListView_SetCheckState(g_app.hList, i, TRUE);
                break;
            }
        }
    }
}

/* ---------------- 数据源 ---------------- */

/* 端口视图数据源：双栈监听表与全进程快照做 Join，
 * 并追加 winnat/系统保留区间行（EACCES 诊断） */
static int BuildPortRows(void)
{
    ProcList all;
    NetList nl;
    PortRangeList rl;
    size_t i, k;

    free(g_app.rows);
    g_app.rows = NULL;
    g_app.rowCount = 0;

    ZeroMemory(&all, sizeof(all));
    ZeroMemory(&nl, sizeof(nl));
    ZeroMemory(&rl, sizeof(rl));
    ScanAllProcesses(&all);
    if (ScanListenPorts(&nl) < 0) {
        FreeProcList(&all);
        FreeNetList(&nl);
        FreePortRangeList(&rl);
        return -1;
    }
    ScanReservedPortRanges(&rl); /* 尽力而为：失败当无数据 */
    g_app.rows = (PortRow *)malloc((nl.count + rl.count ? nl.count + rl.count : 1) *
                                   sizeof(PortRow));
    if (!g_app.rows) {
        FreeProcList(&all);
        FreeNetList(&nl);
        FreePortRangeList(&rl);
        return -1;
    }
    for (i = 0; i < nl.count; i++) {
        PortRow *r = &g_app.rows[i];

        r->pid = nl.items[i].pid;
        r->port = nl.items[i].port;
        r->tcp = nl.items[i].tcp;
        r->ipv6 = nl.items[i].ipv6;
        r->found = FALSE;
        r->reserved = FALSE;
        r->portEnd = 0;
        ZeroMemory(&r->proc, sizeof(r->proc));
        for (k = 0; k < all.count; k++) {
            if (all.items[k].pid == r->pid) {
                r->proc = all.items[k];
                r->found = TRUE;
                break;
            }
        }
    }
    for (i = 0; i < rl.count; i++) {
        PortRow *r = &g_app.rows[nl.count + i];

        ZeroMemory(r, sizeof(*r));
        r->reserved = TRUE;
        r->port = rl.items[i].start;
        r->portEnd = rl.items[i].end;
        r->tcp = rl.items[i].tcp;
        r->ipv6 = rl.items[i].ipv6;
    }
    g_app.rowCount = nl.count + rl.count;
    FreeProcList(&all);
    FreeNetList(&nl);
    FreePortRangeList(&rl);
    return (int)g_app.rowCount;
}

/* ---------------- 行渲染 ---------------- */

static int GetIconIndex(const WCHAR *path)
{
    SHFILEINFOW fi;

    ZeroMemory(&fi, sizeof(fi));
    if (path && path[0] &&
        SHGetFileInfoW(path, 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
        return fi.iIcon;
    return -1;
}

static void FormatMem(unsigned long long bytes, WCHAR *buf, size_t cch)
{
    StringCchPrintfW(buf, cch, L"%lu.%lu MB",
                     (unsigned long)(bytes / 1048576ULL),
                     (unsigned long)((bytes % 1048576ULL) * 10ULL / 1048576ULL));
}

/* ---------------- 列排序（数据层排序，刷新后保持） ---------------- */

static int s_cmpCol;
static BOOL s_cmpDesc;

static int CmpPid(DWORD a, DWORD b) { return (a > b) - (a < b); }
static int CmpUll(unsigned long long a, unsigned long long b) { return (a > b) - (a < b); }

/* 端口视图协议列排序权重：TCP, TCP6, UDP, UDP6 */
static int ProtoRank(const PortRow *r)
{
    if (r->tcp)
        return r->ipv6 ? 1 : 0;
    return r->ipv6 ? 3 : 2;
}

/* 全部进程 / Node-Python 视图比较器（列定义两视图一致）
 * 列：0 进程名 1 PID 2 父PID 3 内存 4 类型 5 路径 */
static int __cdecl CmpProcRow(const void *pa, const void *pb)
{
    const ProcInfo *x = (const ProcInfo *)pa;
    const ProcInfo *y = (const ProcInfo *)pb;
    int r = 0;

    switch (s_cmpCol) {
    case 0: r = lstrcmpiW(x->name, y->name); break;
    case 1: r = CmpPid(x->pid, y->pid); break;
    case 2: r = CmpPid(x->ppid, y->ppid); break;
    case 3: r = CmpUll(x->memBytes, y->memBytes); break;
    case 4: r = (int)x->type - (int)y->type; break;
    case 5: r = lstrcmpiW(x->path, y->path); break;
    default: r = 0; break;
    }
    if (!r)
        r = CmpPid(x->pid, y->pid); /* PID 唯一键兜底，保证排序结果确定 */
    return s_cmpDesc ? -r : r;
}

/* 端口视图比较器
 * 列：0 端口 1 协议 2 PID 3 进程名 4 类型 5 内存 6 路径
 * 行分层（不受排序方向影响）：正常监听 < 保留区间 < 已退出进程 */
static int PortRowTier(const PortRow *r)
{
    if (r->reserved)
        return 1;
    return r->found ? 0 : 2;
}

static int __cdecl CmpPortRow(const void *pa, const void *pb)
{
    const PortRow *x = (const PortRow *)pa;
    const PortRow *y = (const PortRow *)pb;
    int r = 0;

    if (PortRowTier(x) != PortRowTier(y))
        return PortRowTier(x) - PortRowTier(y);
    switch (s_cmpCol) {
    case 0: r = (x->port > y->port) - (x->port < y->port); break;
    case 1: r = ProtoRank(x) - ProtoRank(y); break;
    case 2: r = CmpPid(x->pid, y->pid); break;
    case 3: r = x->reserved ? 0 : lstrcmpiW(x->proc.name, y->proc.name); break;
    case 4: r = (int)x->proc.type - (int)y->proc.type; break;
    case 5: r = CmpUll(x->proc.memBytes, y->proc.memBytes); break;
    case 6: r = x->reserved ? 0 : lstrcmpiW(x->proc.path, y->proc.path); break;
    default: r = 0; break;
    }
    if (!r)
        r = CmpPid(x->pid, y->pid);
    if (!r)
        r = (x->port > y->port) - (x->port < y->port);
    return s_cmpDesc ? -r : r;
}

/* 日志视图比较器
 * 列：0 时间 1 来源 2 进程名 3 PID 4 结果 5 路径 */
static int __cdecl CmpLogRow(const void *pa, const void *pb)
{
    const LogEntry *x = (const LogEntry *)pa;
    const LogEntry *y = (const LogEntry *)pb;
    int r = 0;

    switch (s_cmpCol) {
    case 0: r = (x->unixTime > y->unixTime) - (x->unixTime < y->unixTime); break;
    case 1: r = lstrcmpiW(x->source, y->source); break;
    case 2: r = lstrcmpiW(x->name, y->name); break;
    case 3: r = CmpPid(x->pid, y->pid); break;
    case 4: r = (int)x->ok - (int)y->ok; break;
    case 5: r = lstrcmpiW(x->path, y->path); break;
    default: r = 0; break;
    }
    if (!r)
        r = (x->unixTime > y->unixTime) - (x->unixTime < y->unixTime);
    return s_cmpDesc ? -r : r;
}

/* 对当前视图缓存数据应用排序（不改列头箭头） */
static void ApplySort(void)
{
    if (g_app.sortCol < 0)
        return;
    if (g_app.mode == MODE_ALL && g_app.treeMode)
        return; /* 树形模式由重建时的兄弟节点排序处理，不平铺排序缓存 */
    s_cmpCol = g_app.sortCol;
    s_cmpDesc = g_app.sortDesc;
    if (g_app.mode == MODE_PORT) {
        if (g_app.rows && g_app.rowCount)
            qsort(g_app.rows, g_app.rowCount, sizeof(PortRow), CmpPortRow);
    } else if (g_app.mode == MODE_LOG) {
        if (g_app.logs.items && g_app.logs.count)
            qsort(g_app.logs.items, g_app.logs.count, sizeof(LogEntry), CmpLogRow);
    } else {
        if (g_app.procs.items && g_app.procs.count)
            qsort(g_app.procs.items, g_app.procs.count, sizeof(ProcInfo), CmpProcRow);
    }
}

/* 刷新列头排序箭头（仅当前排序列显示升/降箭头） */
static void UpdateHeaderArrows(void)
{
    HWND hHdr = g_app.hList ? ListView_GetHeader(g_app.hList) : NULL;
    int n, i;

    if (!hHdr)
        return;
    n = Header_GetItemCount(hHdr);
    for (i = 0; i < n; i++) {
        HDITEMW hdi;
        ZeroMemory(&hdi, sizeof(hdi));
        hdi.mask = HDI_FORMAT;
        if (!Header_GetItem(hHdr, i, &hdi))
            continue;
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_app.sortCol)
            hdi.fmt |= g_app.sortDesc ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(hHdr, i, &hdi);
    }
}

void ViewsSortBy(int col)
{
    if (g_app.mode == MODE_ALL && g_app.treeMode) {
        /* 树形模式：支持列排序（兄弟节点排序，内存列=子树合计）。
         * 内存列首次点击默认降序（最大分组在前），其余列默认升序。 */
        if (g_app.sortCol == col)
            g_app.sortDesc = !g_app.sortDesc;
        else {
            g_app.sortCol = col;
            g_app.sortDesc = (col == 3);
        }
        ViewsRebuild();
        UpdateHeaderArrows();
        return;
    }
    if (g_app.sortCol == col)
        g_app.sortDesc = !g_app.sortDesc;
    else {
        g_app.sortCol = col;
        g_app.sortDesc = FALSE;
    }
    ApplySort();
    ViewsRebuild();
    UpdateHeaderArrows();
}

/* ---------------- 树形排序 ---------------- */

/* 树形模式兄弟节点排序上下文（ViewsRebuild 树形分支内设置） */
static const long long *s_treeSubMem;
static int s_treeCmpCol = -1;
static BOOL s_treeCmpDesc;

static int __cdecl TreeIdxCmp(const void *pa, const void *pb)
{
    int x = *(const int *)pa;
    int y = *(const int *)pb;
    const ProcInfo *px = &g_app.procs.items[x];
    const ProcInfo *py = &g_app.procs.items[y];
    int r = 0;

    switch (s_treeCmpCol) {
    case 0: r = lstrcmpiW(px->name, py->name); break;
    case 1: r = CmpPid(px->pid, py->pid); break;
    case 3: /* 内存列 = 子树合计（分组占用） */
        r = (s_treeSubMem[x] > s_treeSubMem[y]) - (s_treeSubMem[x] < s_treeSubMem[y]);
        break;
    case 5: r = lstrcmpiW(px->path, py->path); break;
    default: r = 0; break;
    }
    return s_treeCmpDesc ? -r : r;
}

/* 树形模式：切换某 PID 子树折叠状态并重建 */
void ViewsToggleCollapse(DWORD pid)
{
    int i;

    if (!(g_app.mode == MODE_ALL && g_app.treeMode))
        return;
    for (i = 0; i < g_app.collapsedCount; i++)
        if (g_app.collapsedPids[i] == pid) {
            g_app.collapsedPids[i] =
                g_app.collapsedPids[g_app.collapsedCount - 1];
            g_app.collapsedCount--;
            ViewsRebuild();
            return;
        }
    if (g_app.collapsedCount < 128)
        g_app.collapsedPids[g_app.collapsedCount++] = pid;
    ViewsRebuild();
}

/* 从缓存重建列表（应用当前筛选），不做系统快照 */
void ViewsRebuild(void)
{
    DWORD *keepPids;
    size_t keepN = 0;
    WCHAR filter[64];
    int nNode = 0, nPy = 0, nOther = 0, shown = 0, nTcp = 0, nUdp = 0;
    int nReservedShown = 0;
    int row = 0;

    if (!g_app.hList)
        return;
    if (g_app.mode == MODE_DIAG) {
        ListView_DeleteAllItems(g_app.hList);
        return; /* WP5: 诊断页签无列表行 */
    }
    GetFilterText(filter, 64);

    SaveCheckedPids(&keepPids, &keepN);
    SendMessageW(g_app.hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_app.hList);

    if (g_app.mode == MODE_PORT) {
        for (size_t i = 0; i < g_app.rowCount; i++) {
            PortRow *r = &g_app.rows[i];
            const WCHAR *proto;
            WCHAR port[16], pid[16], mem[32];
            LVITEMW lvi;
            int idx;

            if (r->reserved) {
                if (!PortRangeMatch(r->port, r->portEnd, filter))
                    continue;
                proto = r->tcp ? (r->ipv6 ? L"TCP6" : L"TCP")
                               : (r->ipv6 ? L"UDP6" : L"UDP");
                StringCchPrintfW(port, 16, L"%lu-%lu",
                                 (unsigned long)r->port, (unsigned long)r->portEnd);
                ZeroMemory(&lvi, sizeof(lvi));
                lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
                lvi.iItem = row++;
                lvi.pszText = port;
                lvi.lParam = 0; /* 保留区间行无 PID */
                lvi.iImage = -1;
                idx = ListView_InsertItem(g_app.hList, &lvi);
                if (idx < 0)
                    continue;
                ListView_SetItemText(g_app.hList, idx, 1, (LPWSTR)proto);
                ListView_SetItemText(g_app.hList, idx, 2, (LPWSTR)L"-");
                ListView_SetItemText(g_app.hList, idx, 3, (LPWSTR)L"(系统保留端口区间)");
                ListView_SetItemText(g_app.hList, idx, 4, (LPWSTR)L"-");
                ListView_SetItemText(g_app.hList, idx, 5, (LPWSTR)L"-");
                ListView_SetItemText(g_app.hList, idx, 6, (LPWSTR)L"-");
                nReservedShown++;
                shown++;
                if (r->tcp)
                    nTcp++;
                else
                    nUdp++;
                continue;
            }
            if (!PortMatch(r->port, filter))
                continue;
            proto = r->tcp ? (r->ipv6 ? L"TCP6" : L"TCP")
                           : (r->ipv6 ? L"UDP6" : L"UDP");
            StringCchPrintfW(port, 16, L"%lu", (unsigned long)r->port);
            StringCchPrintfW(pid, 16, L"%lu", (unsigned long)r->pid);
            if (r->found)
                FormatMem(r->proc.memBytes, mem, 32);
            else
                StringCchCopyW(mem, 32, L"-");

            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            lvi.iItem = row++;
            lvi.pszText = port;
            lvi.lParam = (LPARAM)(INT_PTR)r->pid;
            lvi.iImage = GetIconIndex(r->found ? r->proc.path : NULL);
            idx = ListView_InsertItem(g_app.hList, &lvi);
            if (idx < 0)
                continue;
            ListView_SetItemText(g_app.hList, idx, 1, (LPWSTR)proto);
            ListView_SetItemText(g_app.hList, idx, 2, pid);
            ListView_SetItemText(g_app.hList, idx, 3,
                                 (LPWSTR)(r->found ? r->proc.name : L"(进程已退出)"));
            ListView_SetItemText(g_app.hList, idx, 4,
                                 (LPWSTR)(r->found ? ProcTypeLabel(r->proc.type) : L"-"));
            ListView_SetItemText(g_app.hList, idx, 5, mem);
            ListView_SetItemText(g_app.hList, idx, 6,
                                 (LPWSTR)(r->found
                                              ? (r->proc.path[0] ? r->proc.path : L"(无法读取)")
                                              : L"-"));
            shown++;
            if (r->tcp)
                nTcp++;
            else
                nUdp++;
        }
    } else if (g_app.mode == MODE_LOG) {
        /* 日志视图：时间/来源/进程名/PID/结果/路径 */
        int nOk = 0;
        for (size_t i = 0; i < g_app.logs.count; i++) {
            LogEntry *e = &g_app.logs.items[i];
            WCHAR pid[16];
            LVITEMW lvi;
            int idx;

            if (filter[0] &&
                !NameMatch(e->name, filter) && !NameMatch(e->source, filter) &&
                !NameMatch(e->path, filter))
                continue;
            StringCchPrintfW(pid, 16, L"%lu", (unsigned long)e->pid);
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            lvi.iItem = row++;
            lvi.pszText = e->timeText;
            lvi.lParam = (LPARAM)(INT_PTR)e->pid;
            lvi.iImage = -1;
            idx = ListView_InsertItem(g_app.hList, &lvi);
            if (idx < 0)
                continue;
            ListView_SetItemText(g_app.hList, idx, 1, e->source);
            ListView_SetItemText(g_app.hList, idx, 2, e->name);
            ListView_SetItemText(g_app.hList, idx, 3, pid);
            ListView_SetItemText(g_app.hList, idx, 4,
                                 (LPWSTR)(e->ok ? L"已终止" : L"失败"));
            ListView_SetItemText(g_app.hList, idx, 5,
                                 e->path[0] ? e->path : (LPWSTR)L"-");
            if (e->ok)
                nOk++;
            shown++;
        }
        nNode = nOk; /* 复用变量位传给状态栏 */
    } else {
        /* 全部进程 / Node-Python 视图共用行渲染 */
        BOOL tree = (g_app.mode == MODE_ALL && g_app.treeMode);
        BOOL cmdCol = (g_app.mode == MODE_PROC); /* Node/Python 视图第 7 列 */
        if (tree) {
            /* 树形模式：DFS 展开（父在前子紧随），折叠子树整体隐藏。
             * 支持列排序：兄弟节点按当前排序列排序（内存列 = 子树合计）。 */
            int n = (int)g_app.procs.count;
            int *parentIdx = (int *)malloc((size_t)(n ? n : 1) * sizeof(int));
            long long *subMem = (long long *)malloc((size_t)(n ? n : 1) * sizeof(long long));
            int *firstKid = (int *)malloc((size_t)(n ? n : 1) * sizeof(int));
            int *nextSib = (int *)malloc((size_t)(n ? n : 1) * sizeof(int));
            int *kidBuf = (int *)malloc((size_t)(n ? n : 1) * sizeof(int));

            if (parentIdx && subMem && firstKid && nextSib && kidBuf) {
                /* 建父子链 */
                for (int i = 0; i < n; i++) {
                    parentIdx[i] = -1;
                    firstKid[i] = -1;
                    nextSib[i] = -1;
                    subMem[i] = (long long)g_app.procs.items[i].memBytes;
                }
                for (int i = 0; i < n; i++) {
                    ProcInfo *p = &g_app.procs.items[i];
                    if (p->pid == 0 || p->pid == 4)
                        continue;
                    for (int pi = 0; pi < n; pi++) {
                        if (g_app.procs.items[pi].pid == p->ppid &&
                            g_app.procs.items[pi].pid != 0) {
                            parentIdx[i] = pi;
                            break;
                        }
                    }
                }
                for (int i = 0; i < n; i++) {
                    if (parentIdx[i] >= 0) {
                        nextSib[i] = firstKid[parentIdx[i]];
                        firstKid[parentIdx[i]] = i;
                    }
                }

                /* 排序设置（内存列用子树合计） */
                s_treeSubMem = subMem;
                s_treeCmpCol = g_app.sortCol;
                s_treeCmpDesc = g_app.sortDesc;

                /* 收集某节点的孩子到 kidBuf 并按排序规则排序，返回数量 */
                #define TREE_COLLECT_KIDS(node)                                    \
                    do {                                                           \
                        int kc = 0;                                                \
                        for (int k = firstKid[node]; k >= 0; k = nextSib[k])       \
                            kidBuf[kc++] = k;                                      \
                        if (s_treeCmpCol >= 0 && kc > 1)                           \
                            qsort(kidBuf, (size_t)kc, sizeof(int), TreeIdxCmp);    \
                        nkids = kc;                                                \
                    } while (0)

                /* DFS 根节点列表也排序 */
                {
                    int rc = 0;
                    int *roots = kidBuf; /* 复用缓冲（根处理先于孩子使用） */
                    for (int i = 0; i < n; i++) {
                        if (g_app.procs.items[i].pid == 0 ||
                            g_app.procs.items[i].pid == 4)
                            continue;
                        if (parentIdx[i] < 0)
                            roots[rc++] = i;
                    }
                    if (s_treeCmpCol >= 0 && rc > 1)
                        qsort(roots, (size_t)rc, sizeof(int), TreeIdxCmp);

                    {
                        int stack[128];
                        int dstack[128];
                        int sp = 0;
                        int nkids = 0;
                        for (int r = rc - 1; r >= 0; r--) {
                            stack[sp] = roots[r];
                            dstack[sp] = 0;
                            if (sp < 127)
                                sp++;
                        }
                        while (sp > 0) {
                            int cur, d, idx;
                            ProcInfo *cp;
                            WCHAR pid[16], ppid[16], mem[32], name[128];
                            LVITEMW lvi;
                            BOOL collapsed = FALSE;
                            unsigned long long memShown;

                            sp--;
                            cur = stack[sp];
                            d = dstack[sp];
                            cp = &g_app.procs.items[cur];

                            /* 折叠状态 */
                            for (int c = 0; c < g_app.collapsedCount; c++)
                                if (g_app.collapsedPids[c] == cp->pid)
                                    collapsed = TRUE;

                            /* 孩子数量（未折叠时用于 ▾/▸ 标记） */
                            {
                                int kc = 0;
                                for (int k = firstKid[cur]; k >= 0; k = nextSib[k])
                                    kc++;
                                nkids = kc;
                            }

                            /* 内存列显示：有孩子时显示子树合计 */
                            memShown = (nkids > 0)
                                           ? (unsigned long long)subMem[cur]
                                           : cp->memBytes;

                            if (!NameMatch(cp->name, filter))
                                goto dfs_skip_row;
                            name[0] = L'\0';
                            for (int s = 0; s < d && s < 16; s++)
                                StringCchCatW(name, 128, L"    ");
                            if (nkids > 0)
                                StringCchCatW(name, 128, collapsed ? L"▸ " : L"▾ ");
                            else
                                StringCchCatW(name, 128, L"· ");
                            StringCchCatNW(name, 128, cp->name, 64);
                            StringCchPrintfW(pid, 16, L"%lu", (unsigned long)cp->pid);
                            StringCchPrintfW(ppid, 16, L"%lu", (unsigned long)cp->ppid);
                            FormatMem(memShown, mem, 32);
                            ZeroMemory(&lvi, sizeof(lvi));
                            lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
                            lvi.iItem = row++;
                            lvi.pszText = name;
                            lvi.lParam = (LPARAM)(INT_PTR)cp->pid;
                            lvi.iImage = GetIconIndex(cp->path);
                            idx = ListView_InsertItem(g_app.hList, &lvi);
                            if (idx >= 0) {
                                ListView_SetItemText(g_app.hList, idx, 1, pid);
                                ListView_SetItemText(g_app.hList, idx, 2, ppid);
                                ListView_SetItemText(g_app.hList, idx, 3, mem);
                                ListView_SetItemText(g_app.hList, idx, 4,
                                    (LPWSTR)((cp->type == PT_NONE) ? L"—"
                                                                   : ProcTypeLabel(cp->type)));
                                ListView_SetItemText(g_app.hList, idx, 5,
                                    cp->path[0] ? cp->path : (LPWSTR)L"(无法读取)");
                            }
                            shown++;
                            if (cp->type == PT_NODE)
                                nNode++;
                            else if (cp->type == PT_PYTHON)
                                nPy++;
                            else
                                nOther++;

                        dfs_skip_row:
                            if (collapsed)
                                continue; /* 折叠：不压子节点 */
                            TREE_COLLECT_KIDS(cur);
                            for (int c = nkids - 1; c >= 0 && sp < 127; c--) {
                                stack[sp] = kidBuf[c];
                                dstack[sp] = d + 1;
                                sp++;
                            }
                        }
                    }
                }
                #undef TREE_COLLECT_KIDS
                s_treeSubMem = NULL;
            }
            free(parentIdx);
            free(subMem);
            free(firstKid);
            free(nextSib);
            free(kidBuf);
        } else {
        for (size_t i = 0; i < g_app.procs.count; i++) {
            ProcInfo *p = &g_app.procs.items[i];
            WCHAR pid[16], ppid[16], mem[32];
            const WCHAR *typeText;
            LVITEMW lvi;
            int idx;

            if (g_app.mode == MODE_PROC && p->type == PT_NONE)
                continue; /* Node/Python 视图只显示目标类型 */
            if (!NameMatch(p->name, filter))
                continue;
            StringCchPrintfW(pid, 16, L"%lu", (unsigned long)p->pid);
            StringCchPrintfW(ppid, 16, L"%lu", (unsigned long)p->ppid);
            FormatMem(p->memBytes, mem, 32);
            typeText = (p->type == PT_NONE) ? L"—" : ProcTypeLabel(p->type);

            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            lvi.iItem = row++;
            lvi.pszText = p->name;
            lvi.lParam = (LPARAM)(INT_PTR)p->pid;
            lvi.iImage = GetIconIndex(p->path);
            idx = ListView_InsertItem(g_app.hList, &lvi);
            if (idx < 0)
                continue;
            ListView_SetItemText(g_app.hList, idx, 1, pid);
            ListView_SetItemText(g_app.hList, idx, 2, ppid);
            ListView_SetItemText(g_app.hList, idx, 3, mem);
            ListView_SetItemText(g_app.hList, idx, 4, (LPWSTR)typeText);
            ListView_SetItemText(g_app.hList, idx, 5,
                                 p->path[0] ? p->path : (LPWSTR)L"(无法读取)");
            if (cmdCol) {
                ListView_SetItemText(g_app.hList, idx, 6,
                                     p->cmdline[0] ? p->cmdline : (LPWSTR)L"-");
                /* WP3: AI 风险列 */
                {
                    int riskCol = 7;
                    ListView_SetItemText(g_app.hList, idx, riskCol,
                        (LPWSTR)(p->aiRisk == RISK_HIGH ? L"高"
                            : (p->aiRisk == RISK_MED ? L"中"
                            : (p->aiRisk == RISK_LOW ? L"低" : L"—"))));
                }
            } else {
                ListView_SetItemText(g_app.hList, idx, 6,
                    (LPWSTR)(p->aiRisk == RISK_HIGH ? L"高"
                        : (p->aiRisk == RISK_MED ? L"中"
                        : (p->aiRisk == RISK_LOW ? L"低" : L"—"))));
            }

            if (p->type == PT_NODE)
                nNode++;
            else if (p->type == PT_PYTHON)
                nPy++;
            else
                nOther++;
            shown++;
        }
        }
    }
    RestoreCheckedPids(keepPids, keepN);
    free(keepPids);
    SendMessageW(g_app.hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hList, NULL, TRUE);

    if (g_app.hStatus) {
        WCHAR s[192];
        if (g_app.mode == MODE_ALL) {
            if (g_app.treeMode)
                StringCchPrintfW(s, 192,
                                 L"共 %d 进程（树形）    Node: %d    Python: %d    内存列=子树合计    %02d:%02d:%02d",
                                 (int)g_app.procs.count, nNode, nPy,
                                 (int)g_app.lastScan.wHour, (int)g_app.lastScan.wMinute,
                                 (int)g_app.lastScan.wSecond);
            else
                StringCchPrintfW(s, 192,
                                 L"共 %d 个进程（显示 %d）    Node: %d    Python: %d    上次刷新 %02d:%02d:%02d",
                                 (int)g_app.procs.count, shown, nNode, nPy,
                                 (int)g_app.lastScan.wHour, (int)g_app.lastScan.wMinute,
                                 (int)g_app.lastScan.wSecond);
        }
        else if (g_app.mode == MODE_PROC)
            StringCchPrintfW(s, 192,
                             L"Node.js: %d 个    Python: %d 个    上次刷新 %02d:%02d:%02d",
                             nNode, nPy, (int)g_app.lastScan.wHour,
                             (int)g_app.lastScan.wMinute, (int)g_app.lastScan.wSecond);
        else if (g_app.mode == MODE_LOG)
            StringCchPrintfW(s, 192,
                             L"终止记录: %d 条（显示 %d）    成功 %d / 失败 %d    上次刷新 %02d:%02d:%02d",
                             (int)g_app.logs.count, shown, nNode, shown - nNode,
                             (int)g_app.lastScan.wHour, (int)g_app.lastScan.wMinute,
                             (int)g_app.lastScan.wSecond);
        else
            StringCchPrintfW(s, 192,
                             L"监听: %d 项（显示 %d）  保留区间: %d 个    TCP %d / UDP %d    上次刷新 %02d:%02d:%02d",
                             (int)g_app.rowCount - nReservedShown, shown - nReservedShown,
                             nReservedShown, nTcp, nUdp,
                             (int)g_app.lastScan.wHour, (int)g_app.lastScan.wMinute,
                             (int)g_app.lastScan.wSecond);
        SendMessageW(g_app.hStatus, SB_SETTEXTW, 0, (LPARAM)s);
    }
}

/* 重做系统快照并重建列表（按当前模式取数） */
void ViewsRescan(void)
{
    GetLocalTime(&g_app.lastScan);

    if (g_app.mode == MODE_DIAG) {
        return; /* WP5: 诊断页签无快照需求 */
    }
    if (g_app.mode == MODE_PORT) {
        if (BuildPortRows() < 0) {
            if (g_app.hStatus)
                SendMessageW(g_app.hStatus, SB_SETTEXTW, 0,
                             (LPARAM)L"端口监听表获取失败，请稍后刷新");
            return;
        }
    } else if (g_app.mode == MODE_LOG) {
        KlogFree(&g_app.logs);
        KlogLoad(&g_app.logs); /* 失败按空处理 */
    } else {
        FreeProcList(&g_app.procs);
        if (g_app.mode == MODE_ALL) {
            if (ScanAllProcesses(&g_app.procs) < 0) {
                if (g_app.hStatus)
                    SendMessageW(g_app.hStatus, SB_SETTEXTW, 0,
                                 (LPARAM)L"进程快照创建失败，请稍后刷新");
                return;
            }
        } else {
            if (ScanProcesses(&g_app.procs) < 0) {
                if (g_app.hStatus)
                    SendMessageW(g_app.hStatus, SB_SETTEXTW, 0,
                                 (LPARAM)L"进程快照创建失败，请稍后刷新");
                return;
            }
        }
    }
    ApplySort(); /* 刷新后保持当前排序 */
    ViewsRebuild();
}

/* 读取页签当前选中项，切换模式 */
void ViewsApplyMode(BOOL rescan)
{
    int mode = MODE_ALL;

    if (g_app.hTab)
        mode = (int)TabCtrl_GetCurSel(g_app.hTab);
    if (mode == (int)g_app.mode)
        return;
    g_app.mode = (ListMode)mode;
    g_app.sortCol = -1;  /* 列集不同，切换视图重置排序 */
    g_app.sortDesc = FALSE;
    ViewsSetColumns();
    if (g_app.hEditFilter)
        SendMessageW(g_app.hEditFilter, EM_SETCUEBANNER, FALSE,
                     (LPARAM)(mode == MODE_PORT
                                  ? L"筛选端口，如：3000 或 80,3000-3010"
                                  : (mode == MODE_LOG
                                         ? L"筛选名称/来源/路径"
                                         : L"筛选进程名，如：node")));
    if (rescan)
        ViewsRescan();
}

void ViewsCleanup(void)
{
    FreeProcList(&g_app.procs);
    free(g_app.rows);
    g_app.rows = NULL;
    g_app.rowCount = 0;
    KlogFree(&g_app.logs);
}
