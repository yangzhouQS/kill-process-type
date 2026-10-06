/* actions.c - 用户动作实现：终止进程、复制路径、列表右键菜单
 * 依赖 views.c 的 ViewsRescan() 在动作完成后刷新界面。
 */
#include "common.h"
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "app.h"
#include "resource.h"
#include "config.h"
#include "klog.h"
#include "monitor.h"
#include "net.h"
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
                AppendMenuW(m, MF_STRING, IDM_LIST_SMART_RESTART,
                            L"智能重启（杀后原参数拉起）");
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

/* ---------------- WP3: 批量 AI 风险扫描 ---------------- */

static BOOL s_batchActive = FALSE;

/* 从模型 JSON 解析风险等级字符串 */
static RiskLevel ParseRiskLevel(const WCHAR *s)
{
    if (!s)
        return RISK_UNKNOWN;
    if (wcsstr(s, L"高"))
        return RISK_HIGH;
    if (wcsstr(s, L"中"))
        return RISK_MED;
    if (wcsstr(s, L"低"))
        return RISK_LOW;
    return RISK_UNKNOWN;
}

void ActionsAiBatchScan(HWND hwnd)
{
    int cnt, checked = 0;
    WCHAR context[16384];
    WCHAR prompt[18432];
    size_t ctxLen = 0;
    int collected = 0;

    (void)hwnd;
    if (AiBusy() || s_batchActive)
        return;
    if (!g_app.hList || (g_app.mode != MODE_ALL && g_app.mode != MODE_PROC))
        return;

    context[0] = L'\0';
    cnt = ListView_GetItemCount(g_app.hList);
    for (int i = 0; i < cnt && collected < 50; i++) {
        if (!ListView_GetCheckState(g_app.hList, i))
            continue;
        checked++;
        {
            LVITEMW it;
            DWORD pid;
            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_PARAM;
            it.iItem = i;
            if (!ListView_GetItem(g_app.hList, &it))
                continue;
            pid = (DWORD)it.lParam;
            if (!pid)
                continue;
            /* 从缓存取上下文 */
            const ProcInfo *pi = FindInfoByPid(pid);
            if (pi) {
                WCHAR line[512];
                StringCchPrintfW(line, 512, L"%lu\t%ls\t%luKB\t%ls\t%ls\t-\r\n",
                                 (unsigned long)pid, pi->name,
                                 (unsigned long)(pi->memBytes >> 10),
                                 pi->type == PT_NODE ? L"node"
                                     : (pi->type == PT_PYTHON ? L"python" : L"-"),
                                 pi->path);
                if (ctxLen + lstrlenW(line) < 16384 - 64) {
                    StringCchCatW(context, 16384, line);
                    ctxLen = lstrlenW(context);
                }
            }
            collected++;
        }
    }
    if (checked == 0) {
        MessageBoxW(NULL, L"请先勾选要扫描的进程。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    StringCchPrintfW(prompt, 18432, AiGetPrompt(AIPROMPT_RISK_BATCH), context);
    AiTruncateContext(prompt, 18432);

    s_batchActive = TRUE;
    {
        WCHAR st[128];
        StringCchPrintfW(st, 128, L"AI 风险扫描中…（%d 个进程，约 40~90 秒）",
                         collected);
        if (g_app.hStatus)
            SendMessageW(g_app.hStatus, SB_SETTEXTW, 0, (LPARAM)st);
    }
    if (!AiStartAnalysis(g_app.hMain, prompt)) {
        s_batchActive = FALSE;
        MessageBoxW(NULL, L"无法启动 kilo 批量分析。", L"错误",
                    MB_OK | MB_ICONERROR);
    }
}

/* WM_APP_AI_DONE 时：若批量扫描进行中，解析结果回填 */
void ActionsAiBatchApply(WPARAM wp, LPARAM lp)
{
    AiResult *r;

    if (!s_batchActive)
        return;
    s_batchActive = FALSE;
    r = (AiResult *)lp;
    if (!r || !wp || !r->answer) {
        if (g_app.hStatus)
            SendMessageW(g_app.hStatus, SB_SETTEXTW, 0,
                         (LPARAM)L"AI 风险扫描失败（kilo 不可用或超时）。");
        return;
    }
    {
        WCHAR *json = AiExtractJson(r->answer);
        if (json) {
            /* 解析 [{pid, level, reason}] 数组 */
            /* 简化解析：扫描 "pid":NUM ... "level":"X" */
            const WCHAR *p = json;
            int filled = 0;
            while (p && *p) {
                const WCHAR *pidPos = wcsstr(p, L"\"pid\":");
                const WCHAR *lvlPos;
                if (!pidPos)
                    break;
                {
                    DWORD pid = (DWORD)_wtol(pidPos + 6);
                    lvlPos = wcsstr(pidPos, L"\"level\":");
                    if (lvlPos && lvlPos - pidPos < 200) {
                        RiskLevel lv;
                        const WCHAR *q = lvlPos + 8;
                        WCHAR levelStr[8] = {0};
                        int li = 0;
                        if (*q == L'"')
                            q++;
                        while (*q && *q != L'"' && li < 7)
                            levelStr[li++] = *q++;
                        lv = ParseRiskLevel(levelStr);
                        /* 回填缓存 */
                        for (size_t k = 0; k < g_app.procs.count; k++) {
                            if (g_app.procs.items[k].pid == pid) {
                                g_app.procs.items[k].aiRisk = lv;
                                filled++;
                                break;
                            }
                        }
                        p = lvlPos + 8;
                    } else {
                        p = pidPos + 6;
                    }
                }
            }
            free(json);
            if (g_app.hStatus) {
                WCHAR st[128];
                StringCchPrintfW(st, 128, L"AI 风险扫描完成：%d 个进程已评级。",
                                 filled);
                SendMessageW(g_app.hStatus, SB_SETTEXTW, 0, (LPARAM)st);
            }
            TrayShowBalloon(MAIN_WINDOW_TITLE, L"AI 风险扫描完成。");
        } else {
            if (g_app.hStatus)
                SendMessageW(g_app.hStatus, SB_SETTEXTW, 0,
                             (LPARAM)L"AI 风险扫描：模型输出非结构化（显示为未知）。");
        }
    }
    ViewsRebuild(); /* 刷新风险列 */
}

/* ---------------- WP5: AI 全局诊断 ---------------- */

static HWND s_diagDlg;
static HWND s_diagBtn, s_diagStatus, s_diagEdit;

/* ---------------- WP5: AI 全局诊断 ---------------- */

static HWND s_diagDlg;
static HWND s_diagBtn, s_diagStatus, s_diagEdit;
static BOOL s_diagPending = FALSE; /* kilo 正在为诊断窗口分析 */

/* 组装全局诊断上下文 */
static void DiagBuildContext(WCHAR *buf, size_t cch)
{
    ProcList l;
    NetList nl;
    PortRangeList rl;
    ProcList orphans;

    buf[0] = L'\0';
    StringCchCatW(buf, cch, L"=== 进程树（Top 30） ===\n");
    ZeroMemory(&l, sizeof(l));
    if (ScanAllProcesses(&l) >= 0) {
        for (size_t i = 0; i < l.count && i < 30; i++)
        {
            WCHAR line[256];
            StringCchPrintfW(line, 256, L"%lu\t%ls\t%luKB\tppid=%lu\t%ls\n",
                             (unsigned long)l.items[i].pid, l.items[i].name,
                             (unsigned long)(l.items[i].memBytes >> 10),
                             (unsigned long)l.items[i].ppid,
                             l.items[i].path[0] ? l.items[i].path : L"-");
            StringCchCatW(buf, cch, line);
        }
    }

    StringCchCatW(buf, cch, L"\n=== 端口监听（Top 30） ===\n");
    ZeroMemory(&nl, sizeof(nl));
    ScanListenPorts(&nl);
    for (size_t i = 0; i < nl.count && i < 30; i++) {
        WCHAR line[128];
        StringCchPrintfW(line, 128, L"%lu\t%ls\tpid=%lu\n",
                         (unsigned long)nl.items[i].port,
                         nl.items[i].tcp ? L"TCP" : L"UDP",
                         (unsigned long)nl.items[i].pid);
        StringCchCatW(buf, cch, line);
    }

    StringCchCatW(buf, cch, L"\n=== winnat 保留区间 ===\n");
    ZeroMemory(&rl, sizeof(rl));
    ScanReservedPortRanges(&rl);
    for (size_t i = 0; i < rl.count; i++) {
        WCHAR line[64];
        StringCchPrintfW(line, 64, L"%lu-%lu\t%ls\n",
                         (unsigned long)rl.items[i].start,
                         (unsigned long)rl.items[i].end,
                         rl.items[i].tcp ? L"TCP" : L"UDP");
        StringCchCatW(buf, cch, line);
    }

    StringCchCatW(buf, cch, L"\n=== 孤儿进程（Node/Python） ===\n");
    ZeroMemory(&orphans, sizeof(orphans));
    ScanOrphanProcesses(&orphans, TRUE);
    if (orphans.count == 0)
        StringCchCatW(buf, cch, L"（无）\n");
    for (size_t i = 0; i < orphans.count; i++) {
        WCHAR line[256];
        StringCchPrintfW(line, 256, L"pid=%lu\t%ls\t%ls\n",
                         (unsigned long)orphans.items[i].pid,
                         orphans.items[i].name, orphans.items[i].path);
        StringCchCatW(buf, cch, line);
    }

    StringCchCatW(buf, cch, L"\n=== 内存 Top 10 ===\n");
    /* 用已有进程列表做选择排序 Top 10 */
    if (l.count > 0) {
        /* 按内存降序取前 10 */
        for (int rank = 0; rank < 10 && (size_t)rank < l.count; rank++) {
            size_t maxIdx = (size_t)rank;
            for (size_t j = rank + 1; j < l.count; j++)
                if (l.items[j].memBytes > l.items[maxIdx].memBytes)
                    maxIdx = j;
            if (maxIdx != (size_t)rank) {
                ProcInfo t = l.items[rank];
                l.items[rank] = l.items[maxIdx];
                l.items[maxIdx] = t;
            }
            WCHAR line[256];
            StringCchPrintfW(line, 256, L"%lu\t%ls\t%luKB\n",
                             (unsigned long)l.items[rank].pid,
                             l.items[rank].name,
                             (unsigned long)(l.items[rank].memBytes >> 10));
            StringCchCatW(buf, cch, line);
        }
    }

    StringCchCatW(buf, cch, L"\n=== 最近 50 条终止日志 ===\n");
    {
        LogList logs;
        ZeroMemory(&logs, sizeof(logs));
        KlogLoad(&logs);
        for (size_t i = 0; i < logs.count && i < 50; i++) {
            WCHAR line[256];
            StringCchPrintfW(line, 256, L"%ls\t%ls\t%ls\tpid=%lu\t%ls\n",
                             logs.items[i].timeText, logs.items[i].source,
                             logs.items[i].name,
                             (unsigned long)logs.items[i].pid,
                             logs.items[i].ok ? L"已终止" : L"失败");
            StringCchCatW(buf, cch, line);
        }
        KlogFree(&logs);
    }

    FreeProcList(&l);
    FreeNetList(&nl);
    FreePortRangeList(&rl);
    FreeProcList(&orphans);
    AiTruncateContext(buf, 12000);
}

/* 诊断结果应用：渲染报告 + 解析动作按钮 */
static void DiagApplyResult(AiResult *r)
{
    if (!s_diagDlg || !r) {
        s_diagPending = FALSE;
        return;
    }
    s_diagPending = FALSE;
    if (r->answer && r->answer[0]) {
        SetWindowTextW(s_diagStatus, L"诊断完成。");
        char *rtf = RichTextBuildAiReport(
            L"全局系统快照（进程/端口/孤儿/日志）",
            r->thinking, r->answer, r->diag);
        if (rtf) {
            RichTextSetRtf(s_diagEdit, rtf);
            free(rtf);
        } else {
            SetWindowTextW(s_diagEdit, r->answer);
        }
        TrayShowBalloon(MAIN_WINDOW_TITLE, L"AI 全局诊断完成。");

        /* 解析 ACTIONS:[...] 生成动态按钮 */
        {
            const WCHAR *acts = wcsstr(r->answer, L"ACTIONS:");
            if (acts) {
                WCHAR *json = AiExtractJson(acts + 8);
                if (json) {
                    int btnY = 0;
                    const WCHAR *p = json;
                    while (p && *p && btnY < 5) {
                        const WCHAR *actPos = wcsstr(p, L"\"action\":\"");
                        if (!actPos)
                            break;
                        {
                            const WCHAR *q = actPos + 10;
                            WCHAR action[64] = {0};
                            int ai2 = 0;
                            while (*q && *q != L'"' && ai2 < 63)
                                action[ai2++] = *q++;
                            if (wcscmp(action, L"clean_orphans") == 0) {
                                HWND b = CreateWindowExW(0, L"BUTTON",
                                    L"清理孤儿进程",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    10, 10 + btnY * 35, 140, 30,
                                    s_diagDlg, (HMENU)(INT_PTR)(100 + btnY),
                                    g_app.hInst, NULL);
                                if (g_app.hFont)
                                    SendMessageW(b, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
                                btnY++;
                            } else if (wcsncmp(action, L"fix_winnat", 10) == 0) {
                                HWND b = CreateWindowExW(0, L"BUTTON",
                                    L"修复 winnat 端口",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    10, 10 + btnY * 35, 140, 30,
                                    s_diagDlg, (HMENU)(INT_PTR)(101 + btnY),
                                    g_app.hInst, NULL);
                                if (g_app.hFont)
                                    SendMessageW(b, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
                                btnY++;
                            }
                            p = q;
                        }
                    }
                    free(json);
                }
            }
        }
    } else {
        SetWindowTextW(s_diagStatus, L"诊断失败（kilo 不可用或超时）。");
        SetWindowTextW(s_diagEdit,
            L"诊断失败。请确认 kilo 已安装且已登录（kilo auth）。\r\n"
            L"或稍后重试。");
    }
}

/* 导出诊断报告为 .md */
static void DiagExport(void)
{
    if (!s_diagEdit)
        return;
    {
        OPENFILENAMEW ofn;
        WCHAR path[MAX_PATH];
        SYSTEMTIME st;

        GetLocalTime(&st);
        StringCchPrintfW(path, MAX_PATH,
                         L"diagnose-%04u%02u%02u-%02u%02u%02u.md",
                         st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond);
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = s_diagDlg;
        ofn.lpstrFilter = L"Markdown (*.md)\0*.md\0All (*.*)\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameW(&ofn)) {
            /* 从 RichEdit 取纯文本 */
            GETTEXTLENGTHEX gtl;
            GETTEXTEX gt;
            gtl.flags = GTL_DEFAULT;
            gtl.codepage = CP_UTF8;
            {
                LONGLONG len = SendMessageW(s_diagEdit, EM_GETTEXTLENGTHEX,
                                            (WPARAM)&gtl, 0);
                if (len > 0 && len < 1024 * 1024) {
                    char *txt = (char *)malloc((size_t)len + 16);
                    if (txt) {
                        gt.cb = (DWORD)len + 8;
                        gt.flags = GT_DEFAULT;
                        gt.codepage = CP_UTF8;
                        gt.lpDefaultChar = NULL;
                        gt.lpUsedDefChar = NULL;
                        SendMessageW(s_diagEdit, EM_GETTEXTEX,
                                     (WPARAM)&gt, (LPARAM)txt);
                        {
                            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0,
                                                   NULL, CREATE_ALWAYS,
                                                   FILE_ATTRIBUTE_NORMAL, NULL);
                            if (f != INVALID_HANDLE_VALUE) {
                                DWORD w;
                                WriteFile(f, txt, (DWORD)lstrlenA(txt), &w, NULL);
                                CloseHandle(f);
                                SetWindowTextW(s_diagStatus, L"报告已导出。");
                            }
                        }
                        free(txt);
                    }
                }
            }
        }
    }
}

static void DiagLayout(void)
{
    if (!s_diagDlg)
        return;
    {
        RECT rc;
        int pad = AppScale(10);
        GetClientRect(s_diagDlg, &rc);
        MoveWindow(s_diagBtn, pad, pad, AppScale(160), AppScale(30), TRUE);
        MoveWindow(s_diagStatus, pad + AppScale(170), pad + AppScale(6),
                   rc.right - pad * 2 - AppScale(170), AppScale(24), TRUE);
        MoveWindow(s_diagEdit, pad, pad + AppScale(40),
                   rc.right - pad * 2, rc.bottom - pad * 2 - AppScale(40), TRUE);
    }
}

static LRESULT CALLBACK DiagProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        DiagLayout();
        return 0;
    case WM_ERASEBKGND:
        if (ThemeOnEraseBkgnd(hwnd, (HDC)wp))
            return 1;
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HBRUSH br = ThemeOnCtlColor((HWND)lp, (HDC)wp);
        if (br)
            return (LRESULT)br;
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 1) { /* 生成按钮 */
            if (s_diagPending) {
                SetWindowTextW(s_diagStatus, L"诊断进行中，请等待…");
                return 0;
            }
            {
                WCHAR context[16384];
                WCHAR prompt[18432];
                DiagBuildContext(context, 16384);
                StringCchPrintfW(prompt, 18432,
                                 AiGetPrompt(AIPROMPT_DIAG_GLOBAL), context);
                s_diagPending = TRUE;
                SetWindowTextW(s_diagStatus,
                    L"AI 全局诊断中…（kilo 无头执行，约 1~3 分钟）");
                SetWindowTextW(s_diagEdit, L"等待 kilo 返回…");
                if (!AiStartAnalysis(g_app.hMain, prompt)) {
                    s_diagPending = FALSE;
                    SetWindowTextW(s_diagStatus, L"启动诊断失败。");
                }
            }
        } else if (LOWORD(wp) == 2) { /* 导出 */
            DiagExport();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (hwnd == s_diagDlg) {
            s_diagDlg = NULL;
            s_diagEdit = NULL;
            s_diagStatus = NULL;
            s_diagBtn = NULL;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ActionsAiDiagOpen(HWND hwnd)
{
    static const WCHAR DIAG_CLASS[] = L"KptDiagDlg";
    (void)hwnd;
    if (s_diagDlg) {
        ShowWindow(s_diagDlg, SW_RESTORE);
        SetForegroundWindow(s_diagDlg);
        return;
    }
    {
        WNDCLASSEXW wc;
        static BOOL registered = FALSE;
        if (!registered) {
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DiagProc;
            wc.hInstance = g_app.hInst;
            wc.hIcon = LoadIconW(g_app.hInst, MAKEINTRESOURCEW(IDI_APP));
            wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = NULL;
            wc.lpszClassName = DIAG_CLASS;
            if (RegisterClassExW(&wc))
                registered = TRUE;
        }
    }
    {
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (g_app.hMain) {
            RECT rm;
            GetWindowRect(g_app.hMain, &rm);
            x = rm.left + AppScale(60);
            y = rm.top + AppScale(40);
        }
        s_diagDlg = CreateWindowExW(0, DIAG_CLASS, L"AI 全局诊断",
                                    WS_OVERLAPPEDWINDOW,
                                    x, y, AppScale(640), AppScale(520),
                                    g_app.hMain, NULL, g_app.hInst, NULL);
    }
    if (!s_diagDlg)
        return;
    s_diagBtn = CreateWindowExW(0, L"BUTTON", L"生成全局诊断快照",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, AppScale(160), AppScale(30),
                                s_diagDlg, (HMENU)(INT_PTR)1, g_app.hInst, NULL);
    s_diagStatus = CreateWindowExW(0, L"STATIC",
                                   L"点击上方按钮开始分析（需 kilo 已登录）",
                                   WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                   0, 0, AppScale(400), AppScale(24),
                                   s_diagDlg, NULL, g_app.hInst, NULL);
    s_diagEdit = RichTextCreate(s_diagDlg, 0);
    if (s_diagEdit)
        SetWindowTextW(s_diagEdit,
            L"AI 全局诊断\r\n\r\n"
            L"点击「生成全局诊断快照」开始分析。\r\n"
            L"报告将展示：问题清单、根因、建议、可执行动作。");
    if (g_app.hFont) {
        SendMessageW(s_diagEdit, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        SendMessageW(s_diagStatus, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        SendMessageW(s_diagBtn, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
    }
    ThemeApplyFrame(s_diagDlg);
    ShowWindow(s_diagDlg, SW_SHOW);
    UpdateWindow(s_diagDlg);
    DiagLayout();
}

void ActionsDiagCheckPending(WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (s_diagPending) {
        AiResult *r = (AiResult *)lp;
        DiagApplyResult(r);
    }
}

/* ---------------- WP10: AI 推荐配置 ---------------- */

void ActionsAiConfigRecommend(HWND hwnd)
{
    WCHAR context[8192];
    WCHAR prompt[12288];
    LogList logs;
    int orphanCount = 0, failCount = 0;

    (void)hwnd;
    if (AiBusy())
        return;

    /* 采集统计数据 */
    context[0] = L'\0';
    ZeroMemory(&logs, sizeof(logs));
    KlogLoad(&logs);
    for (size_t i = 0; i < logs.count; i++) {
        if (wcsstr(logs.items[i].source, L"孤儿"))
            orphanCount++;
        if (!logs.items[i].ok)
            failCount++;
    }
    KlogFree(&logs);

    StringCchPrintfW(context, 8192,
        L"工具使用统计：\n"
        L"- 终止日志总数：%d 条（最近500条内）\n"
        L"- 孤儿进程清理次数：%d\n"
        L"- 终止失败次数：%d\n"
        L"- 当前配置：自动刷新=%ls（间隔%ld秒）、孤儿定时清理=%ls（间隔%ld分钟）、"
        L"气泡通知=%ls、开机自启=%ls\n"
        L"请基于以上数据推荐配置优化。只输出JSON数组，格式：\n"
        L"[{\"key\":\"配置键名\",\"value\":\"推荐值\",\"reason\":\"一句话理由\"}]\n"
        L"可用键：AutoRefresh,AutoRefreshInterval,OrphanAutoEnable,OrphanIntervalMin,"
        L"BalloonNotify,StartMinimized",
        (int)logs.count, orphanCount, failCount,
        ConfigGetBool(L"AutoRefresh", TRUE) ? L"开" : L"关",
        ConfigGetLong(L"AutoRefreshInterval", 10),
        ConfigGetBool(L"OrphanAutoEnable", FALSE) ? L"开" : L"关",
        ConfigGetLong(L"OrphanIntervalMin", 30),
        ConfigGetBool(L"BalloonNotify", TRUE) ? L"开" : L"关",
        ConfigGetBool(L"StartMinimized", FALSE) ? L"开" : L"关");

    StringCchPrintfW(prompt, 12288,
        L"你是Windows工具配置优化专家。%ls", context);

    /* 打开 AI 报告窗口显示推荐 */
    free(s_aiPrompt);
    s_aiPrompt = NULL;
    {
        size_t plen = (size_t)lstrlenW(prompt) + 1;
        s_aiPrompt = (WCHAR *)malloc(plen * sizeof(WCHAR));
        if (s_aiPrompt)
            StringCchCopyW(s_aiPrompt, plen, prompt);
    }
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
    s_aiPid = 0;
    {
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (g_app.hMain) {
            RECT rm;
            GetWindowRect(g_app.hMain, &rm);
            x = rm.left + AppScale(80);
            y = rm.top + AppScale(50);
        }
        s_aiDlg = CreateWindowExW(0, AI_DLG_CLASS, L"AI 配置推荐",
                                  WS_OVERLAPPEDWINDOW,
                                  x, y, AppScale(560), AppScale(440),
                                  g_app.hMain, NULL, g_app.hInst, NULL);
    }
    if (!s_aiDlg)
        return;
    s_aiKillBtn = CreateWindowExW(0, L"BUTTON", L"终止该进程",
                                  WS_CHILD | BS_PUSHBUTTON,
                                  0, 0, AppScale(130), AppScale(30),
                                  s_aiDlg, (HMENU)(INT_PTR)IDAI_KILL, g_app.hInst, NULL);
    EnableWindow(s_aiKillBtn, FALSE);
    s_aiStatus = CreateWindowExW(0, L"STATIC",
                                 L"AI 分析使用数据中…（约 40~90 秒）",
                                 WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 0, 0, AppScale(300), AppScale(30),
                                 s_aiDlg, NULL, g_app.hInst, NULL);
    s_aiEdit = RichTextCreate(s_aiDlg, 0);
    if (s_aiEdit)
        SetWindowTextW(s_aiEdit,
            L"等待 kilo 返回…\r\n\r\n分析完成后将在此展示推荐配置项。");
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
    }
}

/* ---------------- WP13: 智能重启 ---------------- */

void ActionsSmartRestart(HWND hwnd, int rowIdx)
{
    LVITEMW it;
    DWORD pid;
    WCHAR cmd[1024], cwd[MAX_PATH];
    WCHAR msg[512];
    HANDLE h;
    BOOL killed = FALSE;

    (void)hwnd;
    if (!g_app.hList || rowIdx < 0)
        return;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = rowIdx;
    if (!ListView_GetItem(g_app.hList, &it))
        return;
    pid = (DWORD)it.lParam;
    if (!pid)
        return;

    /* 先取原始命令行与工作目录（杀之前） */
    if (!PebQuery(pid, cmd, 1024, cwd, MAX_PATH) || !cmd[0]) {
        MessageBoxW(NULL,
            L"无法读取该进程命令行（权限不足或进程已退出），\n"
            L"无法智能重启。可手动终止后自行启动。",
            L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    StringCchPrintfW(msg, 512,
        L"智能重启进程 PID %lu：\n\n"
        L"命令行：%ls\n"
        L"工作目录：%ls\n\n"
        L"将终止当前进程并以原参数重新启动。继续？",
        (unsigned long)pid, cmd, cwd[0] ? cwd : L"(默认)");
    if (MessageBoxW(NULL, msg, L"智能重启",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    /* 终止 */
    h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        killed = TerminateProcess(h, 0);
        CloseHandle(h);
    }
    if (!killed) {
        MessageBoxW(NULL, L"终止进程失败。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    /* 等待句柄释放 */
    Sleep(500);

    /* 以原参数重新启动 */
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                           CREATE_NEW_CONSOLE, NULL,
                           cwd[0] ? cwd : NULL, &si, &pi)) {
            WCHAR text[128];
            StringCchPrintfW(text, 128, L"已重启 PID %lu → 新 PID %lu。",
                             (unsigned long)pid, (unsigned long)pi.dwProcessId);
            TrayShowBalloon(MAIN_WINDOW_TITLE, text);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            WCHAR err[256];
            StringCchPrintfW(err, 256,
                L"重启失败（错误 %lu）：\n%ls\n\n"
                L"进程已终止但未能重新启动。",
                (unsigned long)GetLastError(), cmd);
            MessageBoxW(NULL, err, L"智能重启失败", MB_OK | MB_ICONWARNING);
        }
    }

    /* 落审计日志 */
    KlogWrite(L"智能重启", NULL, pid, TRUE, 0);
    ViewsRescan();
}

/* ---------------- WP9: AI 清理策略 ---------------- */

/* 三档策略 */
typedef enum {
    STRAT_AUTO = 0,   /* 可安全自动清理 */
    STRAT_MANUAL,     /* 需人工复核 */
    STRAT_FORBID      /* 禁止操作 */
} CleanStrategy;

void ActionsAiCleanStrategy(HWND hwnd)
{
    ProcList orphans;
    WCHAR context[8192];
    WCHAR prompt[12288];
    size_t ctxLen = 0;

    (void)hwnd;
    if (AiBusy())
        return;

    ZeroMemory(&orphans, sizeof(orphans));
    if (ScanOrphanProcesses(&orphans, FALSE) < 0) {
        MessageBoxW(NULL, L"孤儿扫描失败。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (orphans.count == 0) {
        MessageBoxW(NULL, L"未发现孤儿进程。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* 组装上下文 */
    context[0] = L'\0';
    for (size_t i = 0; i < orphans.count && ctxLen < 8100; i++) {
        WCHAR line[512];
        StringCchPrintfW(line, 512, L"pid=%lu\t%ls\t%ls\t%ls\n",
                         (unsigned long)orphans.items[i].pid,
                         orphans.items[i].name,
                         orphans.items[i].path,
                         orphans.items[i].cmdline[0] ? orphans.items[i].cmdline : L"-");
        StringCchCatW(context, 8192, line);
        ctxLen = lstrlenW(context);
    }
    FreeProcList(&orphans);

    StringCchPrintfW(prompt, 12288,
        L"你是Windows进程清理策略专家。对以下孤儿进程给出清理策略分级。"
        L"必须只输出一个JSON数组，格式：\n"
        L"[{\"pid\":123,\"strategy\":\"auto|manual|forbid\",\"reason\":\"一句话\"}]\n"
        L"auto=可安全自动清理（开发残留孤儿）；"
        L"manual=需人工复核（可能承载服务）；"
        L"forbid=禁止操作（系统关键）。孤儿进程清单：\n%ls",
        context);

    /* 打开 AI 报告窗口 */
    free(s_aiPrompt);
    s_aiPrompt = NULL;
    {
        size_t plen = (size_t)lstrlenW(prompt) + 1;
        s_aiPrompt = (WCHAR *)malloc(plen * sizeof(WCHAR));
        if (s_aiPrompt)
            StringCchCopyW(s_aiPrompt, plen, prompt);
    }
    if (s_aiDlg)
        DestroyWindow(s_aiDlg);
    {
        WNDCLASSEXW wc;
        static BOOL registered2 = FALSE;
        if (!registered2) {
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = AiDlgProc;
            wc.hInstance = g_app.hInst;
            wc.hIcon = LoadIconW(g_app.hInst, MAKEINTRESOURCEW(IDI_APP));
            wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = AI_DLG_CLASS;
            if (RegisterClassExW(&wc))
                registered2 = TRUE;
        }
    }
    s_aiPid = 0;
    {
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (g_app.hMain) {
            RECT rm;
            GetWindowRect(g_app.hMain, &rm);
            x = rm.left + AppScale(80);
            y = rm.top + AppScale(50);
        }
        s_aiDlg = CreateWindowExW(0, AI_DLG_CLASS, L"AI 清理策略",
                                  WS_OVERLAPPEDWINDOW,
                                  x, y, AppScale(560), AppScale(440),
                                  g_app.hMain, NULL, g_app.hInst, NULL);
    }
    if (!s_aiDlg)
        return;
    s_aiKillBtn = CreateWindowExW(0, L"BUTTON", L"清理 auto 项",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, AppScale(130), AppScale(30),
                                  s_aiDlg, (HMENU)(INT_PTR)IDAI_KILL, g_app.hInst, NULL);
    s_aiStatus = CreateWindowExW(0, L"STATIC",
                                 L"AI 策略分析中…（约 40~90 秒）",
                                 WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 0, 0, AppScale(300), AppScale(30),
                                 s_aiDlg, NULL, g_app.hInst, NULL);
    s_aiEdit = RichTextCreate(s_aiDlg, 0);
    if (s_aiEdit)
        SetWindowTextW(s_aiEdit,
            L"等待 kilo 返回…\r\n\r\n"
            L"策略分档：\r\n"
            L"  ✅ auto = 可安全自动清理\r\n"
            L"  ⚠️ manual = 需人工复核\r\n"
            L"  ❌ forbid = 禁止操作\r\n\r\n"
            L"分析完成后可点击「清理 auto 项」执行。");
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
    }
}

/* ---------------- WP11: 时序异常告警 ---------------- */

void ActionsAnomalyCheck(void)
{
    ULONGLONG mem[60];
    double cpu[60];

    if (!ConfigGetBool(L"AnomalyWatch", FALSE))
        return;
    if (MonitorCount() == 0)
        return;

    /* 检查缓存中每个 node/python 进程的时序 */
    for (size_t k = 0; k < g_app.procs.count; k++) {
        DWORD pid;
        int n;

        if (g_app.procs.items[k].type == PT_NONE)
            continue;
        pid = g_app.procs.items[k].pid;
        n = MonitorGetSeries(pid, mem, cpu, 60);
        if (n < 6)
            continue; /* 数据不足 */
        {
            ULONGLONG growth = mem[n - 1] - mem[n - 6];
            int increasing = 1;
            for (int j = n - 5; j < n; j++)
                if (mem[j] < mem[j - 1]) {
                    increasing = 0;
                    break;
                }
            if (increasing && growth > 10ULL * 1024 * 1024) {
                WCHAR text[256];
                StringCchPrintfW(text, 256,
                    L"⚠ %ls（PID %lu）内存持续增长：12秒内 +%.1fMB",
                    g_app.procs.items[k].name,
                    (unsigned long)pid,
                    (double)growth / (1024.0 * 1024.0));
                TrayShowBalloon(MAIN_WINDOW_TITLE, text);
            }
        }
    }
}

/* ---------------- WP12: 基线对比 ---------------- */

void ActionsSaveBaseline(HWND hwnd)
{
    ProcList l;
    NetList nl;
    HANDLE f;
    WCHAR path[MAX_PATH];
    WCHAR exe[MAX_PATH];

    (void)hwnd;
    /* 基线存 exe 旁 baseline.txt */
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return;
    {
        WCHAR *slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = 0;
            StringCchPrintfW(path, MAX_PATH, L"%ls\\baseline.txt", exe);
        } else
            return;
    }

    ZeroMemory(&l, sizeof(l));
    ZeroMemory(&nl, sizeof(nl));
    ScanAllProcesses(&l);
    ScanListenPorts(&nl);

    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        MessageBoxW(NULL, L"保存基线失败。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    {
        char utf8[1024];
        int n;
        DWORD w;

        /* 进程清单 */
        for (size_t i = 0; i < l.count; i++) {
            WCHAR line[512];
            StringCchPrintfW(line, 512, L"P\t%lu\t%ls\t%ls\n",
                             (unsigned long)l.items[i].pid,
                             l.items[i].name,
                             l.items[i].path[0] ? l.items[i].path : L"-");
            n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, 1024, NULL, NULL);
            if (n > 1)
                WriteFile(f, utf8, n - 1, &w, NULL);
        }
        /* 端口清单 */
        for (size_t i = 0; i < nl.count; i++) {
            WCHAR line[256];
            StringCchPrintfW(line, 256, L"L\t%lu\t%ls\t%lu\n",
                             (unsigned long)nl.items[i].port,
                             nl.items[i].tcp ? L"TCP" : L"UDP",
                             (unsigned long)nl.items[i].pid);
            n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, 512, NULL, NULL);
            if (n > 1)
                WriteFile(f, utf8, n - 1, &w, NULL);
        }
    }
    CloseHandle(f);
    FreeProcList(&l);
    FreeNetList(&nl);
    TrayShowBalloon(MAIN_WINDOW_TITLE, L"基线快照已保存。");
}

void ActionsCompareBaseline(HWND hwnd)
{
    WCHAR path[MAX_PATH], exe[MAX_PATH];
    HANDLE f;
    char raw[256 * 1024];
    DWORD got = 0;
    ProcList l;
    NetList nl;
    WCHAR report[4096];
    size_t rp = 0;
    int newProcs = 0, goneProcs = 0, newPorts = 0, gonePorts = 0;

    (void)hwnd;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return;
    {
        WCHAR *slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = 0;
            StringCchPrintfW(path, MAX_PATH, L"%ls\\baseline.txt", exe);
        } else
            return;
    }

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        MessageBoxW(NULL, L"请先保存基线快照。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        DWORD sz = GetFileSize(f, NULL);
        if (sz > sizeof(raw) - 1)
            sz = sizeof(raw) - 1;
        ReadFile(f, raw, sz, &got, NULL);
        raw[got] = '\0';
    }
    CloseHandle(f);

    /* 当前快照 */
    ZeroMemory(&l, sizeof(l));
    ZeroMemory(&nl, sizeof(nl));
    ScanAllProcesses(&l);
    ScanListenPorts(&nl);

    report[0] = L'\0';
    StringCchCatW(report, 4096, L"基线对比结果：\r\n\r\n");

    /* 解析基线：P\tpid\tname\tpath / L\tport\tproto\tpid */
    {
        WCHAR wide[256 * 1024];
        int wlen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, wide, 256 * 1024);
        WCHAR *p2;
        wide[wlen] = L'\0';

        /* 检查新增进程（当前有但基线无） */
        StringCchCatW(report, 4096, L"【新增进程】\r\n");
        for (size_t i = 0; i < l.count && rp < 3800; i++) {
            if (l.items[i].pid == 0 || l.items[i].pid == 4)
                continue;
            BOOL found = FALSE;
            for (p2 = wcsstr(wide, L"P\t"); p2; p2 = wcsstr(p2 + 1, L"P\t")) {
                DWORD bPid = _wtol(p2 + 2);
                if (bPid == l.items[i].pid) {
                    found = TRUE;
                    break;
                }
            }
            if (!found) {
                WCHAR line2[256];
                StringCchPrintfW(line2, 256, L"  + %ls (PID %lu)\r\n",
                                 l.items[i].name, (unsigned long)l.items[i].pid);
                StringCchCatW(report, 4096, line2);
                rp = lstrlenW(report);
                newProcs++;
            }
        }
        if (newProcs == 0)
            StringCchCatW(report, 4096, L"  （无）\r\n");

        /* 检查消失进程（基线有但当前无） */
        StringCchCatW(report, 4096, L"\r\n【消失进程】\r\n");
        for (p2 = wcsstr(wide, L"P\t"); p2 && rp < 3900; p2 = wcsstr(p2 + 1, L"P\t")) {
            DWORD bPid = _wtol(p2 + 2);
            if (bPid == 0 || bPid == 4)
                continue;
            BOOL found = FALSE;
            for (size_t i = 0; i < l.count; i++)
                if (l.items[i].pid == bPid) {
                    found = TRUE;
                    break;
                }
            if (!found) {
                WCHAR name[64] = {0};
                WCHAR *tab1 = wcschr(p2 + 2, L'\t');
                if (tab1) {
                    WCHAR *tab2 = wcschr(tab1 + 1, L'\t');
                    if (tab2) {
                        size_t nl2 = (size_t)(tab2 - tab1 - 1);
                        if (nl2 > 63) nl2 = 63;
                        StringCchCopyNW(name, 64, tab1 + 1, nl2 + 1);
                    }
                }
                {
                    WCHAR line2[256];
                    StringCchPrintfW(line2, 256, L"  - %ls (PID %lu)\r\n",
                                     name, (unsigned long)bPid);
                    StringCchCatW(report, 4096, line2);
                    rp = lstrlenW(report);
                    goneProcs++;
                }
            }
        }
        if (goneProcs == 0)
            StringCchCatW(report, 4096, L"  （无）\r\n");

        /* 新增端口 */
        StringCchCatW(report, 4096, L"\r\n【新增端口】\r\n");
        for (size_t i = 0; i < nl.count && rp < 4000; i++) {
            WCHAR pat[32];
            StringCchPrintfW(pat, 32, L"L\t%lu\t", (unsigned long)nl.items[i].port);
            if (!wcsstr(wide, pat)) {
                WCHAR line2[128];
                StringCchPrintfW(line2, 128, L"  + %lu %ls (PID %lu)\r\n",
                                 (unsigned long)nl.items[i].port,
                                 nl.items[i].tcp ? L"TCP" : L"UDP",
                                 (unsigned long)nl.items[i].pid);
                StringCchCatW(report, 4096, line2);
                rp = lstrlenW(report);
                newPorts++;
            }
        }
        if (newPorts == 0)
            StringCchCatW(report, 4096, L"  （无）\r\n");

        /* 消失端口 */
        StringCchCatW(report, 4096, L"\r\n【消失端口】\r\n");
        for (p2 = wcsstr(wide, L"L\t"); p2 && rp < 4050; p2 = wcsstr(p2 + 1, L"L\t")) {
            DWORD bPort = _wtol(p2 + 2);
            BOOL found = FALSE;
            for (size_t i = 0; i < nl.count; i++)
                if (nl.items[i].port == bPort) {
                    found = TRUE;
                    break;
                }
            if (!found) {
                WCHAR line2[128];
                StringCchPrintfW(line2, 128, L"  - %lu\r\n", (unsigned long)bPort);
                StringCchCatW(report, 4096, line2);
                rp = lstrlenW(report);
                gonePorts++;
            }
        }
        if (gonePorts == 0)
            StringCchCatW(report, 4096, L"  （无）\r\n");
    }

    FreeProcList(&l);
    FreeNetList(&nl);

    StringCchPrintfW(report + lstrlenW(report), 128,
                     L"\r\n汇总：新增进程 %d、消失 %d、新增端口 %d、消失 %d",
                     newProcs, goneProcs, newPorts, gonePorts);
    MessageBoxW(NULL, report, L"基线对比", MB_OK | MB_ICONINFORMATION);
}

/* ---------------- WP7: AI 对话面板（含 AI 调用集成） ---------------- */

#define CHAT_MAX_TURNS 6
#define CHAT_TURN_MAX  1024

typedef struct {
    WCHAR user[CHAT_TURN_MAX];
    WCHAR assistant[CHAT_TURN_MAX];
} ChatTurn;

static HWND s_chatPanel;
static HWND s_chatInput, s_chatDisplay, s_chatSend, s_chatClear;
static BOOL s_chatVisible = FALSE;
static BOOL s_chatPending = FALSE;
static ChatTurn s_chatHistory[CHAT_MAX_TURNS];
static int s_chatTurnCount = 0;
static WCHAR s_lastUserMsg[CHAT_TURN_MAX]; /* 暂存最近一条用户输入 */

static void ChatHistoryPush(const WCHAR *user, const WCHAR *assistant)
{
    if (s_chatTurnCount < CHAT_MAX_TURNS) {
        StringCchCopyNW(s_chatHistory[s_chatTurnCount].user, CHAT_TURN_MAX,
                        user ? user : L"", CHAT_TURN_MAX);
        StringCchCopyNW(s_chatHistory[s_chatTurnCount].assistant, CHAT_TURN_MAX,
                        assistant ? assistant : L"", CHAT_TURN_MAX);
        s_chatTurnCount++;
    } else {
        /* 环形：丢最旧，整体前移 */
        memmove(&s_chatHistory[0], &s_chatHistory[1],
                (CHAT_MAX_TURNS - 1) * sizeof(ChatTurn));
        StringCchCopyNW(s_chatHistory[CHAT_MAX_TURNS - 1].user, CHAT_TURN_MAX,
                        user ? user : L"", CHAT_TURN_MAX);
        StringCchCopyNW(s_chatHistory[CHAT_MAX_TURNS - 1].assistant, CHAT_TURN_MAX,
                        assistant ? assistant : L"", CHAT_TURN_MAX);
    }
}

static void ChatHistoryClear(void)
{
    s_chatTurnCount = 0;
    ZeroMemory(s_chatHistory, sizeof(s_chatHistory));
}

/* 构建系统上下文（D2：每次注入进程+端口 Top 20） */
static void ChatBuildSystemContext(WCHAR *buf, size_t cch)
{
    ProcList l;
    NetList nl;

    buf[0] = L'\0';
    StringCchCatW(buf, cch,
        L"你是Windows进程管理助手（kill-process-type内置对话）。"
        L"回答简洁中文，可直接给PID/端口数字。\n\n"
        L"当前系统快照：\n");

    ZeroMemory(&l, sizeof(l));
    ZeroMemory(&nl, sizeof(nl));
    ScanAllProcesses(&l);
    ScanListenPorts(&nl);

    StringCchCatW(buf, cch, L"进程（按内存Top 20）：\n");
    for (int i = 0; i < 20 && i < (int)l.count; i++) {
        WCHAR line[256];
        StringCchPrintfW(line, 256, L"  %lu %ls %luKB %ls\n",
                         (unsigned long)l.items[i].pid, l.items[i].name,
                         (unsigned long)(l.items[i].memBytes >> 10),
                         l.items[i].type == PT_NODE ? L"[node]"
                             : (l.items[i].type == PT_PYTHON ? L"[python]" : L""));
        StringCchCatW(buf, cch, line);
    }
    StringCchCatW(buf, cch, L"\n端口监听（Top 20）：\n");
    for (size_t i = 0; i < nl.count && i < 20; i++) {
        WCHAR line[128];
        StringCchPrintfW(line, 128, L"  %lu %ls pid=%lu\n",
                         (unsigned long)nl.items[i].port,
                         nl.items[i].tcp ? L"TCP" : L"UDP",
                         (unsigned long)nl.items[i].pid);
        StringCchCatW(buf, cch, line);
    }
    FreeProcList(&l);
    FreeNetList(&nl);
    AiTruncateContext(buf, 8000);
}

/* 构建完整 Prompt（D1：JSON 消息数组格式） */
static void ChatBuildPrompt(const WCHAR *question, WCHAR *prompt, size_t cch)
{
    WCHAR sysCtx[8192];
    size_t pos = 0;

    ChatBuildSystemContext(sysCtx, 8192);

    /* JSON 格式：{"messages":[{"role":"system","content":"..."},
       {"role":"user","content":"..."},{"role":"assistant","content":"..."},
       ...,{"role":"user","content":"当前问题"}]} */
    pos += StringCchPrintfW(prompt + pos, cch - pos,
                            L"{\"messages\":[");
    /* system 消息 */
    pos += StringCchPrintfW(prompt + pos, cch - pos,
                            L"{\"role\":\"system\",\"content\":\"");
    /* 转义系统上下文中的引号和反斜杠 */
    {
        const WCHAR *p = sysCtx;
        while (*p && pos < cch - 100) {
            if (*p == L'"') {
                prompt[pos++] = L'\\';
                prompt[pos++] = L'"';
            } else if (*p == L'\\') {
                prompt[pos++] = L'\\';
                prompt[pos++] = L'\\';
            } else if (*p == L'\n') {
                prompt[pos++] = L'\\';
                prompt[pos++] = L'n';
            } else {
                prompt[pos++] = *p;
            }
            p++;
        }
        prompt[pos] = L'\0';
    }
    pos += StringCchPrintfW(prompt + pos, cch - pos, L"\"}");

    /* 历史消息 */
    for (int i = 0; i < s_chatTurnCount && pos < cch - 2048; i++) {
        /* user */
        pos += StringCchPrintfW(prompt + pos, cch - pos,
                                L",{\"role\":\"user\",\"content\":\"");
        {
            const WCHAR *p = s_chatHistory[i].user;
            while (*p && pos < cch - 1024) {
                if (*p == L'"') { prompt[pos++] = L'\\'; prompt[pos++] = L'"'; }
                else if (*p == L'\\') { prompt[pos++] = L'\\'; prompt[pos++] = L'\\'; }
                else if (*p == L'\n') { prompt[pos++] = L'\\'; prompt[pos++] = L'n'; }
                else prompt[pos++] = *p;
                p++;
            }
            prompt[pos] = L'\0';
        }
        pos += StringCchPrintfW(prompt + pos, cch - pos, L"\"}");
        /* assistant */
        pos += StringCchPrintfW(prompt + pos, cch - pos,
                                L",{\"role\":\"assistant\",\"content\":\"");
        {
            const WCHAR *p = s_chatHistory[i].assistant;
            while (*p && pos < cch - 1024) {
                if (*p == L'"') { prompt[pos++] = L'\\'; prompt[pos++] = L'"'; }
                else if (*p == L'\\') { prompt[pos++] = L'\\'; prompt[pos++] = L'\\'; }
                else if (*p == L'\n') { prompt[pos++] = L'\\'; prompt[pos++] = L'n'; }
                else prompt[pos++] = *p;
                p++;
            }
            prompt[pos] = L'\0';
        }
        pos += StringCchPrintfW(prompt + pos, cch - pos, L"\"}");
    }

    /* 当前 user 消息 */
    pos += StringCchPrintfW(prompt + pos, cch - pos,
                            L",{\"role\":\"user\",\"content\":\"");
    {
        const WCHAR *p = question;
        while (*p && pos < cch - 64) {
            if (*p == L'"') { prompt[pos++] = L'\\'; prompt[pos++] = L'"'; }
            else if (*p == L'\\') { prompt[pos++] = L'\\'; prompt[pos++] = L'\\'; }
            else if (*p == L'\n') { prompt[pos++] = L'\\'; prompt[pos++] = L'n'; }
            else prompt[pos++] = *p;
            p++;
        }
        prompt[pos] = L'\0';
    }
    pos += StringCchPrintfW(prompt + pos, cch - pos, L"\"}");
    pos += StringCchPrintfW(prompt + pos, cch - pos, L"]}");
}

/* 发送消息（ChatProc 内调用） */
static void ChatSendMessage(void)
{
    WCHAR input[2048];
    WCHAR prompt[32768]; /* 32KB：JSON 格式比纯文本多 ~30% */

    if (s_chatPending)
        return;
    GetWindowTextW(s_chatInput, input, 2048);
    if (!input[0])
        return;

    /* 显示用户消息 */
    {
        WCHAR line[2200];
        int len;
        StringCchPrintfW(line, 2200, L"你：%ls\r\n\r\n", input);
        len = GetWindowTextLengthW(s_chatDisplay);
        SendMessageW(s_chatDisplay, EM_SETSEL, len, len);
        SendMessageW(s_chatDisplay, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(s_chatDisplay, EM_SCROLLCARET, 0, 0);
    }

    ChatBuildPrompt(input, prompt, 32768);
    AiTruncateContext(prompt, 30000);
    SetWindowTextW(s_chatInput, L"");

    s_chatPending = TRUE;
    {
        WCHAR wait[128];
        int len;
        StringCchCopyW(wait, 128, L"AI 思考中…（kilo 无头执行，约 40~90 秒）\r\n\r\n");
        len = GetWindowTextLengthW(s_chatDisplay);
        SendMessageW(s_chatDisplay, EM_SETSEL, len, len);
        SendMessageW(s_chatDisplay, EM_REPLACESEL, FALSE, (LPARAM)wait);
    }

    if (!AiStartAnalysis(g_app.hMain, prompt)) {
        s_chatPending = FALSE;
        {
            WCHAR err[256];
            int len;
            StringCchCopyW(err, 256,
                L"⚠ AI 调用失败（kilo 不可用或已有任务进行中）。\r\n\r\n");
            len = GetWindowTextLengthW(s_chatDisplay);
            SendMessageW(s_chatDisplay, EM_SETSEL, len, len);
            SendMessageW(s_chatDisplay, EM_REPLACESEL, FALSE, (LPARAM)err);
        }
    }
}

/* 结果路由（gui.c WM_APP_AI_DONE → ActionsChatCheckPending） */
void ActionsChatCheckPending(WPARAM wp, LPARAM lp)
{
    AiResult *r;

    (void)wp;
    if (!s_chatPending)
        return;
    s_chatPending = FALSE;
    r = (AiResult *)lp;
    if (!r || !r->answer || !r->answer[0]) {
        int len = GetWindowTextLengthW(s_chatDisplay);
        SendMessageW(s_chatDisplay, EM_SETSEL, len, len);
        SendMessageW(s_chatDisplay, EM_REPLACESEL, FALSE,
                     (LPARAM)L"⚠ AI 无响应（kilo 超时或异常）。\r\n\r\n");
        return;
    }
    /* 显示 AI 回复 */
    {
        WCHAR line[CHAT_TURN_MAX + 64];
        int len;
        StringCchPrintfW(line, CHAT_TURN_MAX + 64, L"AI：%ls\r\n\r\n", r->answer);
        len = GetWindowTextLengthW(s_chatDisplay);
        SendMessageW(s_chatDisplay, EM_SETSEL, len, len);
        SendMessageW(s_chatDisplay, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(s_chatDisplay, EM_SCROLLCARET, 0, 0);
    }
    /* 存入历史 */
    ChatHistoryPush(s_lastUserMsg, r->answer);
    s_lastUserMsg[0] = L'\0';
}

/* 暂存最近一条用户输入（ChatSendMessage 写入，ChatCheckPending 消费） */
static WCHAR s_lastUserMsg[CHAT_TURN_MAX];

static void ChatLayout(HWND parent)
{
    if (!s_chatPanel)
        return;
    {
        RECT rc;
        int pw = 360;
        GetClientRect(parent, &rc);
        MoveWindow(s_chatPanel, rc.right - pw, 0, pw, rc.bottom, TRUE);
        {
            RECT prc;
            GetClientRect(s_chatPanel, &prc);
            MoveWindow(s_chatDisplay, 4, 4, prc.right - 8, prc.bottom - 100, TRUE);
            MoveWindow(s_chatInput, 4, prc.bottom - 88, prc.right - 8 - 76, 84, TRUE);
            MoveWindow(s_chatSend, prc.right - 68, prc.bottom - 88, 64, 40, TRUE);
            MoveWindow(s_chatClear, prc.right - 68, prc.bottom - 44, 64, 40, TRUE);
        }
    }
}

static LRESULT CALLBACK ChatProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        ChatLayout(GetParent(hwnd));
        return 0;
    case WM_ERASEBKGND:
        if (ThemeOnEraseBkgnd(hwnd, (HDC)wp))
            return 1;
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == 3) { /* 发送按钮 */
            GetWindowTextW(s_chatInput, s_lastUserMsg, CHAT_TURN_MAX);
            ChatSendMessage();
        } else if (LOWORD(wp) == 4) { /* 清空按钮 */
            ChatHistoryClear();
            SetWindowTextW(s_chatDisplay,
                L"对话已清空。\r\n\r\n输入问题后点击「发送」。\r\n");
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        s_chatVisible = FALSE;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ChatPanelToggle(HWND mainHwnd)
{
    static const WCHAR CHAT_CLASS[] = L"KptChatPanel";

    if (!s_chatPanel) {
        WNDCLASSEXW wc;
        RECT rc;
        static BOOL registered = FALSE;

        if (!registered) {
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = ChatProc;
            wc.hInstance = g_app.hInst;
            wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = NULL;
            wc.lpszClassName = CHAT_CLASS;
            if (RegisterClassExW(&wc))
                registered = TRUE;
        }
        GetClientRect(mainHwnd, &rc);
        s_chatPanel = CreateWindowExW(0, CHAT_CLASS, NULL,
                                      WS_CHILD | WS_VISIBLE,
                                      rc.right - 360, 0, 360, rc.bottom,
                                      mainHwnd, NULL, g_app.hInst, NULL);
        if (!s_chatPanel)
            return;
        s_chatDisplay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                            ES_MULTILINE | ES_AUTOVSCROLL,
                                        0, 0, 100, 100,
                                        s_chatPanel, NULL, g_app.hInst, NULL);
        s_chatInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                                      WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                          ES_AUTOVSCROLL,
                                      0, 0, 100, 80,
                                      s_chatPanel, (HMENU)(INT_PTR)2, g_app.hInst, NULL);
        s_chatSend = CreateWindowExW(0, L"BUTTON", L"发送",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 68, 38,
                                     s_chatPanel, (HMENU)(INT_PTR)3, g_app.hInst, NULL);
        s_chatClear = CreateWindowExW(0, L"BUTTON", L"清空",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 68, 38,
                                     s_chatPanel, (HMENU)(INT_PTR)4, g_app.hInst, NULL);
        if (g_app.hFont) {
            SendMessageW(s_chatDisplay, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
            SendMessageW(s_chatInput, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
            SendMessageW(s_chatSend, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
            SendMessageW(s_chatClear, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        }
        SetWindowTextW(s_chatDisplay,
            L"AI 对话面板\r\n\r\n"
            L"输入问题后点击「发送」或按 Ctrl+Enter。\r\n"
            L"支持多轮上下文（最近 6 轮）。\r\n"
            L"每次提问自动附带系统进程/端口快照。\r\n");
        ChatLayout(mainHwnd);
        s_chatVisible = TRUE;
    } else {
        s_chatVisible = !s_chatVisible;
        ShowWindow(s_chatPanel, s_chatVisible ? SW_SHOW : SW_HIDE);
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
