/* actions.c - 用户动作实现：终止进程、复制路径、列表右键菜单
 * 依赖 views.c 的 ViewsRescan() 在动作完成后刷新界面。
 */
#include "common.h"
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "app.h"
#include "resource.h"
#include "config.h"
#include "klog.h"
#include "peb.h"
#include "startup.h"
#include "theme.h"
#include "tray.h"
#include "views.h"
#include "ai.h"
#include "richtext.h"
#include "actions.h"

/* ---------------- 终止进程 ---------------- */

static void ElevatedFixForRow(HWND owner, int rowIdx); /* 前向声明（见提权修复节） */

static void DoKillPidsEx(const WCHAR *source, DWORD *pids,
                         const ProcInfo *const *infos, size_t n)
{
    KillResult kr;
    DWORD *errs = (DWORD *)malloc((n ? n : 1) * sizeof(DWORD));

    KillPids(pids, n, &kr, errs);
    ViewsRescan();

    /* 逐条落日志（infos[i] 可为 NULL） */
    if (errs) {
        for (size_t i = 0; i < n; i++)
            KlogWrite(source, infos ? infos[i] : NULL, pids[i],
                      errs[i] == 0, errs[i]);
        free(errs);
    }

    WCHAR text[128];
    StringCchPrintfW(text, 128, L"已终止 %d/%d 个进程。", kr.okCount, (int)n);
    TrayShowBalloon(MAIN_WINDOW_TITLE, text);

    if (kr.failCount > 0)
        MessageBoxW(NULL, kr.failDetail, L"部分进程终止失败", MB_OK | MB_ICONWARNING);
}

/* 在缓存中按 PID 查 ProcInfo（勾选清理取名称/路径用），找不到返回 NULL */
static const ProcInfo *FindInfoByPid(DWORD pid)
{
    if (g_app.mode != MODE_PORT) {
        for (size_t i = 0; i < g_app.procs.count; i++)
            if (g_app.procs.items[i].pid == pid)
                return &g_app.procs.items[i];
    } else {
        for (size_t i = 0; i < g_app.rowCount; i++)
            if (g_app.rows[i].found && g_app.rows[i].pid == pid)
                return &g_app.rows[i].proc;
    }
    return NULL;
}

void ActionsKillAllOfType(ProcType t)
{
    ProcList tmp;
    DWORD *pids;
    const ProcInfo **infos;
    size_t n = 0;
    WCHAR msg[128];

    ZeroMemory(&tmp, sizeof(tmp));
    if (ScanProcesses(&tmp) < 0)
        return;
    pids = (DWORD *)malloc((tmp.count ? tmp.count : 1) * sizeof(DWORD));
    infos = (const ProcInfo **)malloc((tmp.count ? tmp.count : 1) * sizeof(const ProcInfo *));
    if (!pids || !infos) {
        free(pids);
        free(infos);
        FreeProcList(&tmp);
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    for (size_t i = 0; i < tmp.count; i++) {
        if (tmp.items[i].type == t) {
            pids[n] = tmp.items[i].pid;
            infos[n] = &tmp.items[i];
            n++;
        }
    }
    int total = (int)n;

    if (total == 0) {
        free(pids);
        free(infos);
        FreeProcList(&tmp);
        MessageBoxW(NULL, L"未发现该类型的进程。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    StringCchPrintfW(msg, 128, L"确定终止全部 %ls 进程（共 %d 个）？",
                     ProcTypeLabel(t), total);
    if (MessageBoxW(NULL, msg, L"操作确认",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        free(pids);
        free(infos);
        FreeProcList(&tmp);
        return;
    }
    DoKillPidsEx(L"类型清理", pids, infos, n); /* tmp 此时仍存活，infos 指针有效 */
    free(pids);
    free(infos);
    FreeProcList(&tmp);
}

void ActionsKillSelected(void)
{
    DWORD *pids;
    size_t n = 0;
    int cnt, checked = 0, reservedCnt = 0, firstReservedRow = -1;

    if (!g_app.hList)
        return;
    cnt = ListView_GetItemCount(g_app.hList);
    for (int i = 0; i < cnt; i++)
        if (ListView_GetCheckState(g_app.hList, i))
            checked++;
    if (checked == 0) {
        MessageBoxW(NULL, L"请先勾选要终止的进程。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    pids = (DWORD *)malloc((size_t)checked * sizeof(DWORD));
    const ProcInfo **infos = (const ProcInfo **)malloc((size_t)checked * sizeof(const ProcInfo *));
    if (!pids || !infos) {
        free(pids);
        free(infos);
        MessageBoxW(NULL, L"内存分配失败。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    for (int i = 0; i < cnt && n + (size_t)reservedCnt < (size_t)checked; i++) {
        if (ListView_GetCheckState(g_app.hList, i)) {
            LVITEMW it;
            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_PARAM;
            it.iItem = i;
            if (ListView_GetItem(g_app.hList, &it)) {
                DWORD pid = (DWORD)it.lParam;
                BOOL dup = FALSE;
                if (pid == 0) { /* winnat/系统保留区间行：无进程可终止 */
                    if (firstReservedRow < 0)
                        firstReservedRow = i;
                    reservedCnt++;
                    continue;
                }
                /* 端口视图同一进程可能占多行（多端口），按 PID 去重 */
                for (size_t k = 0; k < n; k++)
                    if (pids[k] == pid) {
                        dup = TRUE;
                        break;
                    }
                if (!dup) {
                    pids[n] = pid;
                    infos[n] = FindInfoByPid(pid);
                    n++;
                }
            }
        }
    }
    if (n == 0 && reservedCnt > 0) {
        free(pids);
        free(infos);
        if (MessageBoxW(NULL,
                L"选中端口属于 Windows 保留区间（winnat / Hyper-V 动态保留），\n"
                L"没有对应进程可结束，杀进程无法释放。\n\n"
                L"是否现在提权一键修复？（UAC 确认后自动执行：\n"
                L"重启 winnat 并将目标端口永久保留）",
                L"端口被系统保留（EACCES）",
                MB_YESNO | MB_ICONQUESTION) == IDYES)
            ElevatedFixForRow(NULL, firstReservedRow);
        return;
    }
    if (n > 1) {
        WCHAR msg[128];
        StringCchPrintfW(msg, 128, L"确定终止选中的 %d 个进程？", (int)n);
        if (MessageBoxW(NULL, msg, L"操作确认",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            free(pids);
            free(infos);
            return;
        }
    }
    DoKillPidsEx(L"勾选清理", pids, infos, n);
    free(pids);
    free(infos);
}

/* ---------------- 复制可执行路径 ---------------- */

static BOOL CopyTextToClipboard(HWND owner, const WCHAR *text)
{
    size_t bytes;
    HGLOBAL h;
    WCHAR *p;
    BOOL ok;

    if (!text || !text[0])
        return FALSE;
    bytes = ((size_t)lstrlenW(text) + 1) * sizeof(WCHAR);
    h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h)
        return FALSE;
    p = (WCHAR *)GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return FALSE;
    }
    CopyMemory(p, text, bytes);
    GlobalUnlock(h);

    if (!OpenClipboard(owner)) {
        GlobalFree(h);
        return FALSE;
    }
    EmptyClipboard();
    ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
    CloseClipboard();
    if (!ok)
        GlobalFree(h); /* 成功时剪贴板接管内存，不可再释放 */
    return ok;
}

static BOOL IsPlaceholderCell(const WCHAR *s)
{
    return !s || !s[0] ||
           wcscmp(s, L"(无法读取)") == 0 ||
           wcscmp(s, L"-") == 0 ||
           wcscmp(s, L"(进程已退出)") == 0;
}

void ActionsCopySelectedPaths(HWND owner)
{
    WCHAR buf[8192];
    int pathCol, nameCol;
    int idx = -1;
    int copied = 0;

    if (g_app.mode == MODE_PORT) {
        pathCol = 6;
        nameCol = 3;
    } else if (g_app.mode == MODE_LOG) {
        pathCol = 5;
        nameCol = 2;
    } else {
        pathCol = 5;
        nameCol = 0;
    }

    if (!g_app.hList)
        return;
    buf[0] = L'\0';
    while ((idx = ListView_GetNextItem(g_app.hList, idx, LVNI_SELECTED)) >= 0) {
        WCHAR cell[MAX_PATH + 8];
        ListView_GetItemText(g_app.hList, idx, pathCol, cell, MAX_PATH + 8);
        if (IsPlaceholderCell(cell)) {
            ListView_GetItemText(g_app.hList, idx, nameCol, cell, MAX_PATH + 8);
            if (IsPlaceholderCell(cell))
                continue;
        }
        if (copied > 0)
            StringCchCatW(buf, 8192, L"\r\n");
        StringCchCatW(buf, 8192, cell);
        copied++;
    }
    if (copied == 0) {
        MessageBoxW(NULL, L"选中行没有可复制的路径。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (CopyTextToClipboard(owner, buf))
        TrayShowBalloon(MAIN_WINDOW_TITLE,
                        copied > 1 ? L"已复制多条路径到剪贴板。"
                                   : L"已复制可执行路径到剪贴板。");
}

/* ---------------- 列表行右键菜单 ---------------- */

/* 右键命中行的行索引（供 AI 分析/Dev 快捷操作使用） */
static int s_fixRowIndex = -1;
static int s_aiRowIndex = -1;

int ActionsGetContextRow(void)
{
    return s_aiRowIndex;
}

/* 复制 winnat 保留端口的修复命令块（从 s_fixRowIndex 行取端口区间） */
void ActionsCopyFix(HWND owner)
{
    WCHAR cell[32];
    WCHAR text[512];
    int start = 0, end = 0, span = 1;

    if (s_fixRowIndex < 0 || !g_app.hList)
        return;
    ListView_GetItemText(g_app.hList, s_fixRowIndex, 0, cell, 32);
    {
        WCHAR *dash = wcschr(cell, L'-');
        if (dash) {
            *dash = L'\0';
            start = _wtoi(cell);
            end = _wtoi(dash + 1);
        } else {
            start = _wtoi(cell);
            end = start;
        }
    }
    if (start < 1 || start > 65535)
        start = end = 8801;
    if (end < start || end > 65535)
        end = start;
    span = end - start + 1;

    StringCchCopyW(text, 512,
        L":: 释放被 winnat 保留的端口（在管理员终端执行）\r\n"
        L"net stop winnat\r\n"
        L"net start winnat\r\n");
    StringCchCatW(text, 512,
        L":: 永久保留端口给本服务，防止再次被随机圈走（须在 winnat 重启后执行）\r\n");
    StringCchPrintfW(cell, 32, L"netsh int ipv4 add excludedportrange protocol=tcp startport=%d numberofports=%d\r\n",
                     start, span);
    StringCchCatW(text, 512, cell);
    StringCchCatW(text, 512,
        L"netsh interface ipv4 show excludedportrange protocol=tcp\r\n");

    if (CopyTextToClipboard(owner, text))
        TrayShowBalloon(MAIN_WINDOW_TITLE,
                        L"已复制修复命令，请在管理员终端粘贴执行。");
    s_fixRowIndex = -1;
}

/* ---------------- 孤儿进程清理 ---------------- */

/* 孤儿 = 父进程已退出仍存活的残留进程（典型：IDE/npm 崩溃后遗留的
 * node/python worker）。定时静默清理仅默认面向 Node/Python，安全可控。 */
void ActionsCleanOrphans(BOOL autoMode)
{
    ProcList orphans;
    DWORD *pids;
    size_t n = 0;
    BOOL nodePyOnly;

    nodePyOnly = ConfigGetBool(L"OrphanNodePyOnly", TRUE);
    ZeroMemory(&orphans, sizeof(orphans));
    if (ScanOrphanProcesses(&orphans, nodePyOnly) < 0) {
        if (!autoMode)
            MessageBoxW(NULL, L"进程快照创建失败，请稍后重试。", L"提示",
                        MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (orphans.count == 0) {
        if (!autoMode)
            MessageBoxW(NULL, nodePyOnly
                              ? L"未发现 Node/Python 孤儿进程。"
                              : L"未发现孤儿进程。",
                        L"提示", MB_OK | MB_ICONINFORMATION);
        FreeProcList(&orphans);
        return;
    }

    pids = (DWORD *)malloc(orphans.count * sizeof(DWORD));
    if (!pids) {
        FreeProcList(&orphans);
        return;
    }

    if (!autoMode) {
        /* 手动：列出受害者清单确认 */
        WCHAR list[1024];
        size_t used = 0;
        list[0] = L'\0';
        for (size_t i = 0; i < orphans.count && used + 64 < 1024; i++) {
            WCHAR line[64];
            StringCchPrintfW(line, 64, L"· %ls（PID %lu）\r\n",
                             orphans.items[i].name, (unsigned long)orphans.items[i].pid);
            StringCchCatW(list, 1024, line);
            used = lstrlenW(list);
        }
        {
            WCHAR msg[1150];
            StringCchPrintfW(msg, 1150,
                             L"发现 %d 个孤儿进程（父进程已退出）：\r\n\r\n%ls\r\n"
                             L"确定全部终止？", (int)orphans.count, list);
            if (MessageBoxW(NULL, msg, L"清理孤儿进程",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                free(pids);
                FreeProcList(&orphans);
                return;
            }
        }
    }

    for (size_t i = 0; i < orphans.count; i++)
        pids[n++] = orphans.items[i].pid;

    {
        KillResult kr;
        DWORD *errs = (DWORD *)malloc((n ? n : 1) * sizeof(DWORD));
        WCHAR text[128];
        KillPids(pids, n, &kr, errs);
        ConfigSetLong(L"OrphanLastClean", (LONG)GetTickCount64() / 1000);
        if (errs) {
            for (size_t i = 0; i < n; i++)
                KlogWrite(autoMode ? L"孤儿·定时" : L"孤儿·手动",
                          &orphans.items[i], pids[i], errs[i] == 0, errs[i]);
            free(errs);
        }
        StringCchPrintfW(text, 128, L"%ls清理孤儿进程：已终止 %d/%d 个。",
                         autoMode ? L"定时" : L"手动", kr.okCount, (int)n);
        TrayShowBalloon(MAIN_WINDOW_TITLE, text);
        if (kr.failCount > 0)
            MessageBoxW(NULL, kr.failDetail, L"部分孤儿进程终止失败",
                        MB_OK | MB_ICONWARNING);
    }
    FreeProcList(&orphans); /* 日志已取完信息，再释放 */
    free(pids);
}

/* ---------------- 一键提权修复（UAC + 管理员 cmd 内执行） ---------------- */

/* 从区间行文本 "8777-8876" 解析出 (起始, 结束) */
static BOOL ParseRangeCell(HWND list, int row, int *rs, int *re)
{
    WCHAR cell[32];
    WCHAR *dash;

    ListView_GetItemText(list, row, 0, cell, 32);
    dash = wcschr(cell, L'-');
    if (dash) {
        *dash = L'\0';
        *rs = _wtoi(cell);
        *re = _wtoi(dash + 1);
    } else {
        *rs = _wtoi(cell);
        *re = *rs;
    }
    if (*rs < 1 || *rs > 65535 || *re < *rs || *re > 65535)
        return FALSE;
    return TRUE;
}

/* 目标端口：筛选框中落在区间内的端口优先，否则取区间起始端口 */
static int ResolveFixPort(int rs, int re)
{
    WCHAR filter[64];
    const WCHAR *p;
    int v = 0;

    if (!g_app.hEditFilter)
        return rs;
    GetWindowTextW(g_app.hEditFilter, filter, 64);
    p = filter;
    while (*p == L' ' || *p == L',')
        p++;
    while (*p >= L'0' && *p <= L'9') {
        v = v * 10 + (*p - L'0');
        p++;
    }
    if (v >= rs && v <= re)
        return v;
    return rs;
}

/* 提权执行修复命令链；返回 0=已启动 1=用户取消 UAC 2=启动失败 */
static int RunElevatedFix(int port)
{
    SHELLEXECUTEINFOW sei;
    WCHAR params[512];

    StringCchPrintfW(params, 512,
        L"/c \"title 修复 winnat 保留端口 & "
        L"net stop winnat& net start winnat& "
        L"netsh int ipv4 add excludedportrange protocol=tcp startport=%d numberofports=1 store=persistent& "
        L"netsh interface ipv4 show excludedportrange protocol=tcp& "
        L"echo.& echo 完成。可关闭本窗口后回到工具刷新端口列表验证。& pause\"", port);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        return (e == ERROR_CANCELLED) ? 1 : 2;
    }
    return 0;
}

/* 对指定保留区间行执行提权修复（含确认弹窗） */
static void ElevatedFixForRow(HWND owner, int rowIdx)
{
    int rs, re, port, rc;
    WCHAR msg[320];

    (void)owner; /* 弹窗与提权均不需要属主窗口 */
    if (!g_app.hList || rowIdx < 0)
        return;
    if (!ParseRangeCell(g_app.hList, rowIdx, &rs, &re))
        return;
    port = ResolveFixPort(rs, re);

    StringCchPrintfW(msg, 320,
        L"将以管理员权限执行（仅一次 UAC 确认，命令由工具自动执行）：\n\n"
        L"  net stop winnat\n"
        L"  net start winnat\n"
        L"  netsh ... add excludedportrange startport=%d numberofports=1 store=persistent\n"
        L"  netsh ... show excludedportrange（验证）\n\n"
        L"目标端口 %d（先在筛选框输入区间内端口可指定其他端口）。\n\n"
        L"继续？", port, port);
    if (MessageBoxW(NULL, msg, L"一键修复保留端口", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    rc = RunElevatedFix(port);
    if (rc == 0)
        TrayShowBalloon(MAIN_WINDOW_TITLE,
                        L"已启动管理员修复窗口，完成后刷新端口列表验证。");
    else if (rc == 2)
        MessageBoxW(NULL, L"提权启动失败，请改用右键菜单的“复制修复命令”。",
                    L"错误", MB_OK | MB_ICONERROR);
    /* rc == 1：用户在 UAC 弹窗中取消，静默即可 */
}

void ActionsElevatedFix(HWND owner)
{
    ElevatedFixForRow(owner, s_fixRowIndex);
    s_fixRowIndex = -1;
}

void ActionsOnListContextMenu(HWND hwnd, LPARAM lp)
{
    POINT pt;
    int idx;
    HMENU m;

    if (!g_app.hList)
        return;
    if (lp == (DWORD)-1) { /* 键盘触发（Shift+F10/Menu 键）：作用于焦点行 */
        RECT rc;
        idx = ListView_GetNextItem(g_app.hList, -1, LVNI_FOCUSED);
        if (idx < 0)
            return;
        rc.left = LVIR_BOUNDS;
        if (!SendMessageW(g_app.hList, LVM_GETITEMRECT, (WPARAM)idx, (LPARAM)&rc))
            return;
        pt.x = (rc.left + rc.right) / 2;
        pt.y = (rc.top + rc.bottom) / 2;
        ClientToScreen(g_app.hList, &pt);
    } else {
        POINT clientPt;
        LVHITTESTINFO ht;
        pt.x = (SHORT)LOWORD(lp);
        pt.y = (SHORT)HIWORD(lp);
        clientPt = pt;
        ScreenToClient(g_app.hList, &clientPt);
        ZeroMemory(&ht, sizeof(ht));
        ht.pt = clientPt;
        idx = ListView_HitTest(g_app.hList, &ht);
        if (idx < 0 || !(ht.flags & LVHT_ONITEM))
            return;
    }
    /* 右键行并入选中集：菜单作用于全部选中行 */
    ListView_SetItemState(g_app.hList, idx,
                          LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);

    m = CreatePopupMenu();
    if (!m)
        return;
    {
        /* lParam == 0 的行是 winnat/系统保留区间行，追加修复项 */
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = idx;
        if (ListView_GetItem(g_app.hList, &it)) {
            if ((DWORD)it.lParam == 0 && g_app.mode == MODE_PORT) {
                /* 仅端口视图存在 lParam==0 的保留区间行 */
                s_fixRowIndex = idx;
                AppendMenuW(m, MF_STRING, IDM_LIST_ELEVATE_FIX,
                            L"一键修复（UAC 提权执行）");
                AppendMenuW(m, MF_STRING, IDM_LIST_COPY_FIX,
                            L"仅复制修复命令");
                AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            } else if ((DWORD)it.lParam != 0 && g_app.mode == MODE_PORT) {
                /* 端口监听行：快捷访问 URL */
                AppendMenuW(m, MF_STRING, IDM_LIST_OPEN_URL,
                            L"浏览器打开 http://localhost:端口");
                AppendMenuW(m, MF_STRING, IDM_LIST_COPY_URL, L"复制 URL");
                AppendMenuW(m, MF_SEPARATOR, 0, NULL);
                s_aiRowIndex = idx;
                AppendMenuW(m, MF_STRING, IDM_LIST_AI_ANALYZE,
                            L"AI 风险评估（kilo）");
                AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            } else if ((DWORD)it.lParam != 0) {
                s_aiRowIndex = idx;
                AppendMenuW(m, MF_STRING, IDM_LIST_AI_ANALYZE,
                            L"AI 风险评估（kilo）");
                AppendMenuW(m, MF_STRING, IDM_LIST_COPY_CMD, L"复制完整命令行");
                AppendMenuW(m, MF_STRING, IDM_LIST_OPEN_IN_TERMINAL,
                            L"在终端打开所在目录");
                AppendMenuW(m, MF_STRING, IDM_LIST_SHOW_IN_EXPLORER,
                            L"在资源管理器中显示");
                AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            }
        }
    }
    AppendMenuW(m, MF_STRING, IDM_LIST_COPY_PATH, L"复制可执行路径");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------- Dev 快捷操作（URL/目录/命令行） ---------------- */

/* 从端口视图行取端口号（列 0 文本 "8080"），失败返回 0 */
static DWORD GetRowPort(int rowIdx)
{
    WCHAR cell[16];
    WCHAR *end = NULL;
    unsigned long v;

    ListView_GetItemText(g_app.hList, rowIdx, 0, cell, 16);
    v = wcstoul(cell, &end, 10);
    if (end == cell || v == 0 || v > 65535)
        return 0;
    return (DWORD)v;
}

void OpenRowUrl(HWND owner, BOOL copyOnly)
{
    DWORD port = GetRowPort(s_aiRowIndex);
    WCHAR url[80];

    if (!port)
        return;
    StringCchPrintfW(url, 80, L"http://localhost:%lu", (unsigned long)port);
    if (copyOnly) {
        if (CopyTextToClipboard(owner, url))
            TrayShowBalloon(MAIN_WINDOW_TITLE, L"已复制 URL。");
    } else {
        ShellExecuteW(owner, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* 行的路径列文本（按视图取列号） */
static BOOL GetRowPath(int rowIdx, WCHAR *buf, size_t cch)
{
    int pathCol = (g_app.mode == MODE_PORT) ? 6 : 5;

    ListView_GetItemText(g_app.hList, rowIdx, pathCol, buf, (int)cch);
    return buf[0] && wcscmp(buf, L"-") != 0 && wcscmp(buf, L"(无法读取)") != 0;
}

void ShowRowInExplorer(int rowIdx)
{
    WCHAR path[MAX_PATH + 8];
    WCHAR args[MAX_PATH + 16];

    if (!GetRowPath(rowIdx, path, MAX_PATH + 8))
        return;
    StringCchPrintfW(args, MAX_PATH + 16, L"/select,\"%ls\"", path);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

void OpenRowTerminal(int rowIdx)
{
    LVITEMW it;
    WCHAR cwd[MAX_PATH];
    WCHAR args[MAX_PATH + 32];

    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = rowIdx;
    if (!ListView_GetItem(g_app.hList, &it) || (DWORD)it.lParam == 0)
        return;
    if (!PebQuery((DWORD)it.lParam, NULL, 0, cwd, MAX_PATH) || !cwd[0]) {
        /* PEB 不可读时退回可执行目录 */
        WCHAR path[MAX_PATH];
        WCHAR *slash;
        if (!GetRowPath(rowIdx, path, MAX_PATH))
            return;
        slash = wcsrchr(path, L'\\');
        if (slash)
            *slash = L'\0';
        StringCchCopyW(cwd, MAX_PATH, path);
    }
    if (!cwd[0])
        return;
    StringCchPrintfW(args, MAX_PATH + 32, L"/K cd /d \"%ls\"", cwd);
    ShellExecuteW(NULL, L"open", L"cmd.exe", args, NULL, SW_SHOWNORMAL);
}

void CopyRowCmdline(HWND owner, int rowIdx)
{
    LVITEMW it;
    WCHAR cmd[1024];
    WCHAR cwd[MAX_PATH];

    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = rowIdx;
    if (!ListView_GetItem(g_app.hList, &it) || (DWORD)it.lParam == 0)
        return;
    if (!PebQuery((DWORD)it.lParam, cmd, 1024, cwd, MAX_PATH) || !cmd[0]) {
        MessageBoxW(NULL, L"无法读取该进程命令行（权限不足或进程已退出）。",
                    L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (CopyTextToClipboard(owner, cmd))
        TrayShowBalloon(MAIN_WINDOW_TITLE, L"已复制完整命令行。");
}

/* ---------------- AI 风险评估（kilo 无头调用） ---------------- */

#define AI_DLG_TIMER 2 /* 等待计时器：1 秒刷新 */

static HWND s_aiDlg, s_aiEdit, s_aiStatus, s_aiKillBtn;
static DWORD s_aiPid = 0;
static WCHAR *s_aiPrompt = NULL;      /* 本次分析的提示词副本（结果窗口展示用） */
static ULONGLONG s_aiStartTick = 0;   /* 分析开始时刻（等待计时与用时统计） */
static const WCHAR AI_DLG_CLASS[] = L"KptAiRiskDlg";

/* 用时格式化：60 秒内 "12.3 秒"，超过则 "2 分 5 秒"（纯整数运算，无 %f） */
static void FormatElapsed(ULONGLONG ms, WCHAR *buf, size_t cch)
{
    if (ms >= 60000)
        StringCchPrintfW(buf, cch, L"%lu 分 %lu 秒",
                         (unsigned long)(ms / 60000),
                         (unsigned long)((ms % 60000) / 1000));
    else
        StringCchPrintfW(buf, cch, L"%lu.%lu 秒",
                         (unsigned long)(ms / 1000),
                         (unsigned long)((ms % 1000) / 100));
}

static void AiDlgLayout(HWND hwnd)
{
    RECT rc;
    int pad = AppScale(10), btnH = AppScale(30);

    if (!s_aiEdit)
        return;
    GetClientRect(hwnd, &rc);
    int y = rc.bottom - pad - btnH;
    MoveWindow(s_aiKillBtn, pad, y, AppScale(130), btnH, TRUE);
    MoveWindow(s_aiStatus, pad + AppScale(130) + pad, y + AppScale(6),
               rc.right - pad * 2 - AppScale(130) - pad, btnH, TRUE);
    MoveWindow(s_aiEdit, pad, pad, rc.right - pad * 2,
               y - pad - pad > 0 ? y - pad - pad : 0, TRUE);
}

static LRESULT CALLBACK AiDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        AiDlgLayout(hwnd);
        return 0;
    case WM_ERASEBKGND:
        if (ThemeOnEraseBkgnd(hwnd, (HDC)wp))
            return 1;
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HBRUSH br = ThemeOnCtlColor((HWND)lp, (HDC)wp);
        if (br)
            return (LRESULT)br;
        break;
    }
    case WM_DRAWITEM:
        if (ThemeOnDrawItem(lp))
            return TRUE;
        break;

    case WM_SETTINGCHANGE:
        /* 系统深浅色切换直达本窗口（跟随系统模式） */
        if (ThemeOnSettingChange((LPCWSTR)lp))
            ActionsAiOnThemeChanged();
        return 0;
    case WM_TIMER:
        if (wp == AI_DLG_TIMER && s_aiStatus && s_aiStartTick) {
            WCHAR st[96], el[32];
            FormatElapsed(GetTickCount64() - s_aiStartTick, el, 32);
            StringCchPrintfW(st, 96, L"AI 分析中… 已等待 %ls（kilo 无头执行）", el);
            SetWindowTextW(s_aiStatus, st);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = AppScale(420);
        mmi->ptMinTrackSize.y = AppScale(320);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDAI_KILL:
            if (s_aiPid) {
                WCHAR msg2[128];
                StringCchPrintfW(msg2, 128, L"确定终止进程 PID %lu？", (unsigned long)s_aiPid);
                if (MessageBoxW(hwnd, msg2, L"操作确认",
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                    DWORD pid = s_aiPid;
                    const ProcInfo *one[1];
                    one[0] = FindInfoByPid(pid);
                    DestroyWindow(hwnd);
                    DoKillPidsEx(L"AI评估", &pid, one, 1);
                }
            }
            break;
        case IDAI_CLOSE:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;
    case WM_DESTROY:
        if (hwnd == s_aiDlg) {
            KillTimer(hwnd, AI_DLG_TIMER);
            s_aiDlg = NULL;
            s_aiEdit = NULL;
            s_aiStatus = NULL;
            s_aiKillBtn = NULL;
        }
        return 0;    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 按行索引取 PID 并组装提示词，创建评估窗口并启动异步分析 */
void ActionsAiAnalyze(int rowIndex)
{
    LVITEMW it;
    DWORD pid;
    ProcList all;
    ProcInfo key, *found = NULL;
    WCHAR prompt[2048], title[128];

    if (AiBusy())
        return;
    if (!g_app.hList || rowIndex < 0)
        return;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = rowIndex;
    if (!ListView_GetItem(g_app.hList, &it))
        return;
    pid = (DWORD)it.lParam;
    if (!pid)
        return;

    ZeroMemory(&key, sizeof(key));
    ZeroMemory(&all, sizeof(all));
    if (ScanAllProcesses(&all) >= 0) {
        for (size_t i = 0; i < all.count; i++)
            if (all.items[i].pid == pid) {
                found = &all.items[i];
                break;
            }
    }
    if (!found) { /* 进程可能刚退出：用列表行兜底信息 */
        StringCchCopyW(key.name, 64, L"(未知)");
        StringCchPrintfW(key.path, MAX_PATH, L"pid=%lu", (unsigned long)pid);
        key.pid = pid;
        found = &key;
    }
    AiBuildPrompt(found, prompt, 2048);
    FreeProcList(&all);

    /* 保存提示词副本供结果展示 */
    free(s_aiPrompt);
    s_aiPrompt = NULL;
    {
        size_t plen = (size_t)lstrlenW(prompt) + 1;
        s_aiPrompt = (WCHAR *)malloc(plen * sizeof(WCHAR));
        if (s_aiPrompt)
            StringCchCopyW(s_aiPrompt, plen, prompt);
    }

    /* 创建（唯一）评估窗口 */
    if (s_aiDlg)
        DestroyWindow(s_aiDlg);
    {
        WNDCLASSEXW wc;
        static BOOL registered = FALSE;
        if (!registered) {
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = AiDlgProc;
            wc.hInstance = g_app.hInst;
            wc.hIcon = LoadIconW(g_app.hInst, MAKEINTRESOURCEW(IDI_APP));
            wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = AI_DLG_CLASS;
            if (RegisterClassExW(&wc))
                registered = TRUE;
        }
    }
    StringCchPrintfW(title, 128, L"AI 风险评估 — %ls（PID %lu）",
                     found->name, (unsigned long)pid);
    s_aiPid = pid;
    {
        /* 锚定主窗口附近：避免 CW_USEDEFAULT 漂到其他 DPI 的屏幕 */
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (g_app.hMain) {
            RECT rm;
            GetWindowRect(g_app.hMain, &rm);
            x = rm.left + AppScale(80);
            y = rm.top + AppScale(50);
        }
        s_aiDlg = CreateWindowExW(0, AI_DLG_CLASS, title,
                                  WS_OVERLAPPEDWINDOW,
                                  x, y,
                                  AppScale(560), AppScale(440),
                                  g_app.hMain, NULL, g_app.hInst, NULL);
    }
    if (!s_aiDlg)
        return;
    s_aiKillBtn = CreateWindowExW(0, L"BUTTON", L"终止该进程",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, AppScale(130), AppScale(30),
                                  s_aiDlg, (HMENU)(INT_PTR)IDAI_KILL, g_app.hInst, NULL);
    EnableWindow(s_aiKillBtn, FALSE);
    s_aiStatus = CreateWindowExW(0, L"STATIC", L"AI 分析中…（kilo 无头执行，约 0.5~2 分钟，可关闭本窗口取消关注）",
                                 WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 0, 0, AppScale(300), AppScale(30),
                                 s_aiDlg, NULL, g_app.hInst, NULL);
    s_aiEdit = RichTextCreate(s_aiDlg, 0);
    if (s_aiEdit)
        SetWindowTextW(s_aiEdit, L"等待 kilo 返回…\r\n\r\n分析完成后将在此展示：提示词 / 思考过程 / 分析结果。");
    else /* Rich Edit 不可用时退回普通 EDIT */
        s_aiEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                   L"等待 kilo 返回…",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                   0, 0, 100, 100,
                                   s_aiDlg, NULL, g_app.hInst, NULL);
    if (g_app.hFont) {
        SendMessageW(s_aiEdit, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        SendMessageW(s_aiStatus, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        SendMessageW(s_aiKillBtn, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
    }
    ShowWindow(s_aiDlg, SW_SHOW);
    UpdateWindow(s_aiDlg);
    AiDlgLayout(s_aiDlg);
    /* AI 评估窗口主题应用（按钮保持系统样式确保可见；RichEdit 保持浅色） */
    ThemeApplyFrame(s_aiDlg);
    InvalidateRect(s_aiDlg, NULL, TRUE);

    /* 启动等待计时（每秒刷新状态栏；完成后显示总用时） */
    s_aiStartTick = GetTickCount64();
    SetTimer(s_aiDlg, AI_DLG_TIMER, 1000, NULL);

    if (!AiStartAnalysis(g_app.hMain, prompt)) {
        KillTimer(s_aiDlg, AI_DLG_TIMER);
        s_aiStartTick = 0;
        SetWindowTextW(s_aiStatus, L"启动分析失败。");
        SetWindowTextW(s_aiEdit, L"无法启动 kilo 分析任务（可能已有任务进行中）。");
    }
}

/* WM_APP_AI_DONE 路由：lp 为堆分配 AiResult，本函数负责释放 */
void ActionsAiDone(WPARAM wp, LPARAM lp)
{
    AiResult *r = (AiResult *)lp;
    WCHAR el[32];

    if (s_aiDlg && s_aiStartTick) {
        KillTimer(s_aiDlg, AI_DLG_TIMER);
        FormatElapsed(GetTickCount64() - s_aiStartTick, el, 32);
    } else
        StringCchCopyW(el, 32, L"?");

    if (!r) {
        if (s_aiDlg)
            SetWindowTextW(s_aiStatus, L"AI 调用异常终止。");
        return;
    }
    if (s_aiDlg) {
        WCHAR st[128];
        char *rtf = RichTextBuildAiReport(s_aiPrompt, r->thinking,
                                          r->answer, r->diag);
        if (rtf) {
            if (wp)
                StringCchPrintfW(st, 128, L"分析完成（kilo），用时 %ls。", el);
            else
                StringCchPrintfW(st, 128, L"AI 调用失败（用时 %ls），详见内容区诊断。", el);
            SetWindowTextW(s_aiStatus, st);
            RichTextSetRtf(s_aiEdit, rtf);
            free(rtf);
        } else {
            StringCchPrintfW(st, 128, L"%ls（用时 %ls）",
                             wp ? L"分析完成" : L"AI 调用失败", el);
            SetWindowTextW(s_aiStatus, st);
            SetWindowTextW(s_aiEdit, r->answer ? r->answer
                                               : (r->diag ? r->diag : L"(无内容)"));
        }
        EnableWindow(s_aiKillBtn, (BOOL)wp);
    }
    s_aiStartTick = 0;
    AiResultFree(r);
}

/* ---------------- WP2: 日志页签 AI 复盘 ---------------- */

void ActionsAiLogReview(HWND hwnd)
{
    int cnt, checked = 0;
    WCHAR context[8192];
    WCHAR prompt[12288];
    WCHAR title[128];
    size_t ctxLen = 0;
    int collected = 0;

    (void)hwnd;
    if (AiBusy())
        return;
    if (!g_app.hList || g_app.mode != MODE_LOG)
        return;

    /* 收集勾选日志行（上限 100 条，防 kilo 上下文过载） */
    context[0] = L'\0';
    cnt = ListView_GetItemCount(g_app.hList);
    for (int i = 0; i < cnt && collected < 100; i++) {
        if (!ListView_GetCheckState(g_app.hList, i))
            continue;
        checked++;
        if (collected >= 100)
            break;
        {
            WCHAR line[512];
            WCHAR time[32], src[32], name[64], pid[16], result[16], path[MAX_PATH];
            ListView_GetItemText(g_app.hList, i, 0, time, 32);
            ListView_GetItemText(g_app.hList, i, 1, src, 32);
            ListView_GetItemText(g_app.hList, i, 2, name, 64);
            ListView_GetItemText(g_app.hList, i, 3, pid, 16);
            ListView_GetItemText(g_app.hList, i, 4, result, 16);
            ListView_GetItemText(g_app.hList, i, 5, path, MAX_PATH);
            StringCchPrintfW(line, 512, L"%ls\t%ls\t%ls\t%ls\t%ls\t%ls\r\n",
                             time, src, name, pid, result, path);
            if (ctxLen + lstrlenW(line) < 8192 - 64) {
                StringCchCatW(context, 8192, line);
                ctxLen = lstrlenW(context);
            }
            collected++;
        }
    }
    if (checked == 0) {
        MessageBoxW(NULL, L"请先勾选要复盘的日志条目。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (checked > 100)
        StringCchCatW(context, 8192, L"…（超出100条已截断）\r\n");

    /* 组装 Prompt */
    StringCchPrintfW(prompt, 12288, AiGetPrompt(AIPROMPT_LOG_REVIEW), context);
    AiTruncateContext(prompt, 12288);

    /* 保存提示词副本 */
    free(s_aiPrompt);
    s_aiPrompt = NULL;
    {
        size_t plen = (size_t)lstrlenW(prompt) + 1;
        s_aiPrompt = (WCHAR *)malloc(plen * sizeof(WCHAR));
        if (s_aiPrompt)
            StringCchCopyW(s_aiPrompt, plen, prompt);
    }

    /* 创建报告窗口（复用 AI 评估对话框机制，s_aiPid=0 隐藏终止按钮） */
    if (s_aiDlg)
        DestroyWindow(s_aiDlg);
    {
        WNDCLASSEXW wc;
        static BOOL registered = FALSE;
        if (!registered) {
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = AiDlgProc;
            wc.hInstance = g_app.hInst;
            wc.hIcon = LoadIconW(g_app.hInst, MAKEINTRESOURCEW(IDI_APP));
            wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = AI_DLG_CLASS;
            if (RegisterClassExW(&wc))
                registered = TRUE;
        }
    }
    StringCchPrintfW(title, 128, L"AI 日志复盘（%d 条记录）", collected);
    s_aiPid = 0; /* 无终止按钮 */
    {
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (g_app.hMain) {
            RECT rm;
            GetWindowRect(g_app.hMain, &rm);
            x = rm.left + AppScale(80);
            y = rm.top + AppScale(50);
        }
        s_aiDlg = CreateWindowExW(0, AI_DLG_CLASS, title,
                                  WS_OVERLAPPEDWINDOW,
                                  x, y, AppScale(560), AppScale(440),
                                  g_app.hMain, NULL, g_app.hInst, NULL);
    }
    if (!s_aiDlg)
        return;
    s_aiKillBtn = CreateWindowExW(0, L"BUTTON", L"终止该进程",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, AppScale(130), AppScale(30),
                                  s_aiDlg, (HMENU)(INT_PTR)IDAI_KILL, g_app.hInst, NULL);
    EnableWindow(s_aiKillBtn, FALSE); /* 日志复盘无终止 */
    s_aiStatus = CreateWindowExW(0, L"STATIC",
                                 L"AI 复盘中…（kilo 无头执行，约 0.5~2 分钟）",
                                 WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 0, 0, AppScale(300), AppScale(30),
                                 s_aiDlg, NULL, g_app.hInst, NULL);
    s_aiEdit = RichTextCreate(s_aiDlg, 0);
    if (s_aiEdit)
        SetWindowTextW(s_aiEdit,
            L"等待 kilo 返回…\r\n\r\n分析完成后将在此展示复盘报告。");
    else
        s_aiEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                   L"等待 kilo 返回…",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                       ES_MULTILINE | ES_READONLY,
                                   0, 0, 100, 100, s_aiDlg, NULL, g_app.hInst, NULL);
    if (g_app.hFont) {
        SendMessageW(s_aiEdit, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        SendMessageW(s_aiStatus, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        SendMessageW(s_aiKillBtn, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
    }
    ShowWindow(s_aiDlg, SW_SHOW);
    UpdateWindow(s_aiDlg);
    AiDlgLayout(s_aiDlg);
    ThemeApplyFrame(s_aiDlg);
    InvalidateRect(s_aiDlg, NULL, TRUE);

    s_aiStartTick = GetTickCount64();
    SetTimer(s_aiDlg, AI_DLG_TIMER, 1000, NULL);
    if (!AiStartAnalysis(g_app.hMain, prompt)) {
        KillTimer(s_aiDlg, AI_DLG_TIMER);
        s_aiStartTick = 0;
        SetWindowTextW(s_aiStatus, L"启动分析失败。");
        SetWindowTextW(s_aiEdit, L"无法启动 kilo 分析任务。");
    }
}

void ActionsAiMenuCommand(HWND hwnd)
{
    (void)hwnd;
    ActionsAiAnalyze(s_aiRowIndex);
    s_aiRowIndex = -1;
}

/* 主题变更广播：AI 评估窗口打开时重新应用（按钮保持系统样式） */
void ActionsAiOnThemeChanged(void)
{
    if (!s_aiDlg)
        return;
    ThemeApplyFrame(s_aiDlg);
    InvalidateRect(s_aiDlg, NULL, TRUE);
}
