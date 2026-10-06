/* gui.c - 主窗口外壳：控件创建（按钮/页签/筛选框/列表/状态栏）、布局、
 * 字体、页签、消息路由、主题应用与配置集成。数据渲染在 views.c，用户动作在 actions.c。
 */
#include "common.h"
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

#include "app.h"
#include "resource.h"
#include "config.h"
#include "startup.h"
#include "theme.h"
#include "settings.h"
#include "tray.h"
#include "views.h"
#include "actions.h"
#include "ai.h"
#include "gui.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define BTN_COUNT 4

static const struct {
    const WCHAR *text;
    int id;
    int width;
} kBtns[BTN_COUNT] = {
    { L"刷新",             IDC_BTN_REFRESH,   80 },
    { L"杀死选中",         IDC_BTN_KILL_SEL, 120 },
    { L"杀死全部Node",     IDC_BTN_KILL_NODE, 150 },
    { L"杀死全部Python",   IDC_BTN_KILL_PY,  160 },
};

/* 全局应用上下文（app.h 中声明，供 views/actions 共享） */
App g_app;

static HWND s_hBtn[BTN_COUNT];
static HWND g_hChkTree;
static HWND g_hBtnAiLog; /* WP2: 日志复盘按钮（仅日志页签可见） */

/* 主窗口全部控件的主题应用（启动与热切换时调用） */
static void ApplyThemeAll(void)
{
    static const INT kOwnerDrawBtns[BTN_COUNT] = {
        IDC_BTN_REFRESH, IDC_BTN_KILL_SEL, IDC_BTN_KILL_NODE, IDC_BTN_KILL_PY
    };

    ThemeApplyFrame(g_app.hMain);
    ThemeApplyListView(g_app.hList);
    ThemeApplyStatusBar(g_app.hStatus);
    ThemeApplyTabControl(g_app.hTab);
    ThemeApplyEdit(g_app.hEditFilter);
    ThemeApplyButtons(g_app.hMain, kOwnerDrawBtns, BTN_COUNT);
    InvalidateRect(g_app.hMain, NULL, TRUE);
}

/* 按配置重置自动刷新定时器 */
static void ResetAutoTimer(HWND hwnd)
{
    LONG interval = ConfigGetLong(L"AutoRefreshInterval", 10);
    LONG orphanMin;

    if (interval < 3)
        interval = 3;
    if (interval > 3600)
        interval = 3600;
    SetTimer(hwnd, TIMER_AUTO_REFRESH, (UINT)(interval * 1000), NULL);

    orphanMin = ConfigGetLong(L"OrphanIntervalMin", 30);
    if (orphanMin < 1)
        orphanMin = 1;
    if (orphanMin > 1440)
        orphanMin = 1440;
    if (ConfigGetBool(L"OrphanAutoEnable", FALSE))
        SetTimer(hwnd, TIMER_ORPHAN, (UINT)(orphanMin * 60000), NULL);
    else
        KillTimer(hwnd, TIMER_ORPHAN);
}

void GuiApplySettings(void)
{
    ApplyThemeAll();
    if (g_app.hMain)
        ResetAutoTimer(g_app.hMain);
}

/* ---------------- 基础 UI ---------------- */

static void CreateFontScaled(void)
{
    if (g_app.hFont)
        DeleteObject(g_app.hFont);
    g_app.hFont = CreateFontW(-AppScale(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static void ApplyFonts(void)
{
    HWND targets[BTN_COUNT + 5];
    int n = 0;
    int i;
    for (i = 0; i < BTN_COUNT; i++)
        targets[n++] = s_hBtn[i];
    targets[n++] = g_app.hChkAuto;
    targets[n++] = g_app.hTab;
    targets[n++] = g_app.hEditFilter;
    targets[n++] = g_app.hList;
    targets[n++] = g_app.hStatus;
    for (i = 0; i < n; i++)
        if (targets[i])
            SendMessageW(targets[i], WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
}

static void Layout(HWND hwnd)
{
    RECT rc;
    int pad, btnH, y, x, listY, statusH, h;

    if (!g_app.hList)
        return;
    GetClientRect(hwnd, &rc);
    pad = AppScale(8);
    btnH = AppScale(30);
    y = pad;
    x = pad;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (s_hBtn[i])
            MoveWindow(s_hBtn[i], x, y, AppScale(kBtns[i].width), btnH, TRUE);
        x += AppScale(kBtns[i].width) + pad;
    }
    if (g_app.hChkAuto)
        MoveWindow(g_app.hChkAuto, x, y, AppScale(150), btnH, TRUE);

    /* 第二行：页签靠左，筛选框靠右（任务管理器式布局） */
    int y2 = y + btnH + pad;
    if (g_app.hTab)
        MoveWindow(g_app.hTab, pad, y2, AppScale(320), btnH, TRUE);
    if (g_hChkTree)
        MoveWindow(g_hChkTree, pad + AppScale(328), y2, AppScale(56), btnH, TRUE);
    if (g_hBtnAiLog)
        MoveWindow(g_hBtnAiLog, pad + AppScale(392), y2, AppScale(120), btnH, TRUE);
    if (g_app.hEditFilter) {
        int fw = AppScale(280);
        int fx = rc.right - pad - fw;
        if (fx < pad + AppScale(520) + pad)
            fx = pad + AppScale(520) + pad;
        MoveWindow(g_app.hEditFilter, fx, y2, fw, btnH, TRUE);
    }

    statusH = 0;
    if (g_app.hStatus) {
        RECT rs;
        SendMessageW(g_app.hStatus, WM_SIZE, 0, 0);
        GetWindowRect(g_app.hStatus, &rs);
        statusH = rs.bottom - rs.top;
    }

    listY = y2 + btnH + pad;
    h = rc.bottom - listY - statusH - pad;
    if (h < 0)
        h = 0;
    MoveWindow(g_app.hList, pad, listY,
               rc.right - pad * 2 > 0 ? rc.right - pad * 2 : 0, h, TRUE);
}

static void CreateControls(HWND hwnd)
{
    /* 第一行：操作按钮 + 自动刷新开关 */
    for (int i = 0; i < BTN_COUNT; i++)
        s_hBtn[i] = CreateWindowExW(0, L"BUTTON", kBtns[i].text,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, AppScale(kBtns[i].width), AppScale(30),
                                    hwnd, (HMENU)(INT_PTR)kBtns[i].id, g_app.hInst, NULL);

    g_app.hChkAuto = CreateWindowExW(0, L"BUTTON", L"自动刷新",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     0, 0, AppScale(150), AppScale(30),
                                     hwnd, (HMENU)(INT_PTR)IDC_CHK_AUTO, g_app.hInst, NULL);
    SendMessageW(g_app.hChkAuto, BM_SETCHECK,
                 ConfigGetBool(L"AutoRefresh", TRUE) ? BST_CHECKED : BST_UNCHECKED, 0);

    /* 第二行：页签（全部进程 / Node-Python / 端口占用） */
    g_app.hTab = CreateWindowExW(0, WC_TABCONTROL, NULL,
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 0, 0, AppScale(320), AppScale(30),
                                 hwnd, (HMENU)(INT_PTR)IDC_TAB, g_app.hInst, NULL);
    {
        static const WCHAR *kTabs[4] = { L"全部进程", L"Node/Python", L"端口占用", L"日志" };
        TCITEMW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.mask = TCIF_TEXT;
        for (int i = 0; i < 4; i++) {
            ti.pszText = (LPWSTR)kTabs[i];
            TabCtrl_InsertItem(g_app.hTab, i, &ti);
        }
    }

    g_hChkTree = CreateWindowExW(0, L"BUTTON", L"树形",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, AppScale(56), AppScale(30),
                                 hwnd, (HMENU)(INT_PTR)IDC_CHK_TREE, g_app.hInst, NULL);
    SendMessageW(g_hChkTree, BM_SETCHECK,
                 ConfigGetBool(L"TreeView", FALSE) ? BST_CHECKED : BST_UNCHECKED, 0);
    g_app.treeMode = ConfigGetBool(L"TreeView", FALSE);
    g_app.collapsedCount = 0;

    g_app.hEditFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        0, 0, AppScale(280), AppScale(30),
                                        hwnd, (HMENU)(INT_PTR)IDC_EDIT_FILTER, g_app.hInst, NULL);
    SendMessageW(g_app.hEditFilter, EM_SETCUEBANNER, FALSE,
                 (LPARAM)L"筛选进程名，如：node");

    g_app.hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, NULL,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                  0, 0, 100, 100,
                                  hwnd, (HMENU)(INT_PTR)IDC_LIST, g_app.hInst, NULL);
    ListView_SetExtendedListViewStyle(g_app.hList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    /* 挂接系统图像列表（小图标）：进程图标与任务管理器显示一致
     * 该列表归系统所有，无需也不得销毁 */
    {
        SHFILEINFOW sfi;
        ZeroMemory(&sfi, sizeof(sfi));
        HIMAGELIST himl = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi),
                                                     SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (himl)
            ListView_SetImageList(g_app.hList, himl, LVSIL_SMALL);
    }

    g_app.hStatus = CreateWindowExW(0, STATUSCLASSNAME, NULL,
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)7, g_app.hInst, NULL);

    /* WP2: 日志复盘按钮（默认隐藏，切到日志页签时显示） */
    g_hBtnAiLog = CreateWindowExW(0, L"BUTTON", L"AI 复盘日志",
                                  WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, AppScale(120), AppScale(30),
                                  hwnd, (HMENU)(INT_PTR)IDC_BTN_AI_LOG, g_app.hInst, NULL);
    if (g_app.hFont)
        SendMessageW(g_hBtnAiLog, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
}

/* ---------------- 消息处理 ---------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_app.hMain = hwnd;
        CreateFontScaled();
        CreateControls(hwnd);
        ApplyFonts();
        Layout(hwnd);
        ViewsSetColumns();
        ResetAutoTimer(hwnd);
        ApplyThemeAll();
        TrayInit(hwnd, g_app.hInst, WM_APP_TRAY);
        TrayAdd();
        ViewsRescan();
        return 0;

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_SHOWWINDOW:
        if (wp && g_app.wasHidden) { /* 从托盘隐藏恢复显示：立即刷新列表 */
            g_app.wasHidden = FALSE;
            ViewsRescan();
        }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = AppScale(720);
        mmi->ptMinTrackSize.y = AppScale(360);
        return 0;
    }

    case WM_DPICHANGED: {
        RECT *sug = (RECT *)lp; /* 系统建议的新窗口矩形 */
        g_app.dpi = HIWORD(wp);
        CreateFontScaled();
        ApplyFonts();
        if (sug)
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(hwnd);
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_AUTO_REFRESH && g_app.hChkAuto &&
            SendMessageW(g_app.hChkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            IsWindowVisible(hwnd)) /* 隐藏驻留时跳过，恢复显示时再刷新 */
            ViewsRescan();
        else if (wp == TIMER_ORPHAN)
            ActionsCleanOrphans(TRUE); /* 定时静默清理孤儿进程 */
        return 0;

    case WM_APP_TRAY:
        return TrayHandleMessage(hwnd, wp, lp);

    case WM_APP_AI_DONE: /* ai.c 工作线程回投：kilo 分析结果 */
        ActionsAiDone(wp, lp);
        return 0;

    case WM_CONTEXTMENU:
        if ((HWND)wp == g_app.hList)
            ActionsOnListContextMenu(hwnd, lp);
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lp;
        LRESULT themeRes = 0;
        if (ThemeOnHeaderNotify(hdr, &themeRes))
            return themeRes; /* 深色表头绘制 */
        if (hdr && hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
            ViewsApplyMode(TRUE);
            /* WP2: 日志复盘按钮仅日志页签可见 */
            if (g_hBtnAiLog) {
                int mode = (int)TabCtrl_GetCurSel(g_app.hTab);
                ShowWindow(g_hBtnAiLog, mode == MODE_LOG ? SW_SHOW : SW_HIDE);
            }
        }
        else if (hdr && hdr->idFrom == IDC_LIST && hdr->code == LVN_COLUMNCLICK)
            ViewsSortBy(((LPNMLISTVIEW)lp)->iSubItem);
        else if (hdr && hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK) {
            /* 树形模式：双击行折叠/展开其子树 */
            NMITEMACTIVATE *nm = (NMITEMACTIVATE *)lp;
            if (nm && nm->iItem >= 0) {
                LVITEMW it;
                ZeroMemory(&it, sizeof(it));
                it.mask = LVIF_PARAM;
                it.iItem = nm->iItem;
                if (ListView_GetItem(g_app.hList, &it))
                    ViewsToggleCollapse((DWORD)it.lParam);
            }
        }
        break; /* 其余通知交给 DefWindowProc，不能吞掉返回值 */
    }

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
        /* 系统深浅色切换：全局热响应（主窗口 + 设置 + AI 窗口联动） */
        if (ThemeOnSettingChange((LPCWSTR)lp)) {
            GuiApplySettings();
            SettingsOnThemeChanged();
            ActionsAiOnThemeChanged();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_REFRESH:
        case IDM_TRAY_REFRESH:
            ViewsRescan();
            break;
        case IDC_BTN_KILL_SEL:
            ActionsKillSelected();
            break;
        case IDM_LIST_COPY_PATH:
            ActionsCopySelectedPaths(hwnd);
            break;
        case IDM_LIST_COPY_FIX:
            ActionsCopyFix(hwnd);
            break;
        case IDM_LIST_ELEVATE_FIX:
            ActionsElevatedFix(hwnd);
            break;
        case IDM_LIST_AI_ANALYZE:
            ActionsAiMenuCommand(hwnd);
            break;
        case IDM_LIST_OPEN_URL:
            OpenRowUrl(hwnd, FALSE);
            break;
        case IDM_LIST_COPY_URL:
            OpenRowUrl(hwnd, TRUE);
            break;
        case IDM_LIST_SHOW_IN_EXPLORER:
            ShowRowInExplorer(ActionsGetContextRow());
            break;
        case IDM_LIST_COPY_CMD:
            CopyRowCmdline(hwnd, ActionsGetContextRow());
            break;
        case IDM_LIST_OPEN_IN_TERMINAL:
            OpenRowTerminal(ActionsGetContextRow());
            break;
        case IDC_EDIT_FILTER:
            if (HIWORD(wp) == EN_CHANGE)
                ViewsRebuild(); /* 输入即筛选：只重绘，不重扫 */
            break;
        case IDC_CHK_AUTO: /* UI 开关与配置双向同步 */
            ConfigSetBool(L"AutoRefresh",
                          SendMessageW(g_app.hChkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_CHK_TREE: /* 树形分组开关（仅全部进程视图生效，持久化） */
            g_app.treeMode = SendMessageW(g_hChkTree, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ConfigSetBool(L"TreeView", g_app.treeMode);
            if (g_app.mode == MODE_ALL) {
                g_app.sortCol = -1;
                g_app.collapsedCount = 0;
                ViewsSetColumns();
                ViewsRebuild();
            }
            break;
        case IDC_BTN_KILL_NODE:
        case IDM_TRAY_KILL_NODE:
            ActionsKillAllOfType(PT_NODE);
            break;
        case IDC_BTN_KILL_PY:
        case IDM_TRAY_KILL_PY:
            ActionsKillAllOfType(PT_PYTHON);
            break;
        case IDM_TRAY_SHOW:
            TrayToggleWindow(hwnd);
            break;
        case IDM_TRAY_SETTINGS:
            SettingsShow();
            break;
        case IDC_BTN_AI_LOG:
            ActionsAiLogReview(hwnd);
            break;
        case IDM_TRAY_ORPHAN:
            ActionsCleanOrphans(FALSE);
            break;
        case IDM_TRAY_AUTOSTART: {
            BOOL ok = StartupIsEnabled() ? StartupDisable() : StartupEnable();
            if (ok)
                TrayShowBalloon(MAIN_WINDOW_TITLE,
                                StartupIsEnabled()
                                    ? L"已开启开机自启动：登录系统后自动驻留托盘。"
                                    : L"已关闭开机自启动。");
            else
                MessageBoxW(NULL, L"设置开机自启动失败（注册表写入被拒绝）。",
                            L"错误", MB_OK | MB_ICONERROR);
            break;
        }
        case IDM_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_app.wasHidden = TRUE;
        if (!g_app.hideTipShown) {
            g_app.hideTipShown = TRUE;
            TrayShowBalloon(MAIN_WINDOW_TITLE,
                            L"程序已最小化到托盘，右键托盘图标可快捷操作。");
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_AUTO_REFRESH);
        /* 主窗口位置持久化（最小化状态不保存） */
        {
            WINDOWPLACEMENT wpl;
            wpl.length = sizeof(wpl);
            if (GetWindowPlacement(hwnd, &wpl) &&
                wpl.showCmd != SW_SHOWMINIMIZED && wpl.showCmd != SW_HIDE) {
                RECT rc = wpl.rcNormalPosition;
                ConfigSetLong(L"WinX", rc.left);
                ConfigSetLong(L"WinY", rc.top);
                ConfigSetLong(L"WinW", rc.right - rc.left);
                ConfigSetLong(L"WinH", rc.bottom - rc.top);
            }
        }
        TrayRemove();
        ViewsCleanup();
        if (g_app.hFont) {
            DeleteObject(g_app.hFont);
            g_app.hFont = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- 对外接口 ---------------- */

BOOL GuiCreateMain(HINSTANCE hInst, int nCmdShow)
{
    WNDCLASSEXW wc;
    HDC dc;

    g_app.hInst = hInst;
    dc = GetDC(NULL);
    g_app.dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = MAIN_WINDOW_CLASS;

    if (!RegisterClassExW(&wc))
        return FALSE;

    CreateWindowExW(0, MAIN_WINDOW_CLASS, MAIN_WINDOW_TITLE,
                    WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, CW_USEDEFAULT, AppScale(980), AppScale(520),
                    NULL, NULL, hInst, NULL);
    if (!g_app.hMain)
        return FALSE;

    /* 恢复上次窗口位置（无记录则保持默认） */
    {
        LONG x = ConfigGetLong(L"WinX", CW_USEDEFAULT);
        LONG y = ConfigGetLong(L"WinY", CW_USEDEFAULT);
        LONG w = ConfigGetLong(L"WinW", 0);
        LONG h = ConfigGetLong(L"WinH", 0);
        if (w >= AppScale(400) && h >= AppScale(300) && x != CW_USEDEFAULT)
            SetWindowPos(g_app.hMain, NULL, x, y, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* SW_HIDE=/tray 参数（带气泡）；SW_SHOWMINNOACTIVE=启动时最小化（静默） */
    if (nCmdShow == SW_SHOWMINNOACTIVE) {
        g_app.wasHidden = TRUE;
        ShowWindow(g_app.hMain, SW_HIDE);
        UpdateWindow(g_app.hMain);
        return TRUE;
    }

    ShowWindow(g_app.hMain, nCmdShow);
    UpdateWindow(g_app.hMain);
    if (nCmdShow == SW_HIDE) {
        g_app.wasHidden = TRUE;
        TrayShowBalloon(MAIN_WINDOW_TITLE,
                        L"程序已在后台运行，左键点击托盘图标可打开主界面。");
    }
    return TRUE;
}
