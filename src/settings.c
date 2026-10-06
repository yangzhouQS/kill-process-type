/* settings.c - 设置窗口实现：分区呈现 自启/启动最小化/主题/自动刷新/气泡通知。
 * 所有改动即时写配置并应用（主题热切换、托盘即时生效）。
 */
#include "common.h"
#include <commctrl.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "app.h"
#include "config.h"
#include "startup.h"
#include "theme.h"
#include "gui.h"
#include "actions.h"
#include "resource.h"
#include "settings.h"

static const WCHAR SETTINGS_CLASS[] = L"KptSettingsDlg";

static HWND s_dlg;
static HWND s_chkAutostart, s_chkStartMin, s_chkAutoEn, s_chkBalloon;
static HWND s_chkOrphanEn, s_edOrphanIv, s_udOrphanIv, s_chkOrphanNp;
static HWND s_rdAuto, s_rdLight, s_rdDark, s_edInterval, s_btnClose, s_udInterval;
static BOOL s_loading = FALSE; /* 载入初值时屏蔽 EN_CHANGE 回写 */
static int s_dpi = 96;        /* 本窗口 DPI（跨屏时独立于主窗口更新） */

static int SC(int v) { return MulDiv(v, s_dpi, 96); }

static void ApplyThemeSelf(void)
{
    /* 注：动态窗口的按钮不做 ownerdraw 深色自绘（实测该场景 WM_DRAWITEM
     * 绘制不生效），保持系统样式确保可见；主窗口按钮不受影响。 */
    ThemeApplyFrame(s_dlg);
    ThemeApplyEdit(s_edInterval);
    InvalidateRect(s_dlg, NULL, TRUE);
}

static void LoadValues(void)
{
    WCHAR buf[16];

    s_loading = TRUE;
    SendMessageW(s_chkAutostart, BM_SETCHECK,
                 StartupIsEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_chkStartMin, BM_SETCHECK,
                 ConfigGetBool(L"StartMinimized", FALSE) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_chkAutoEn, BM_SETCHECK,
                 ConfigGetBool(L"AutoRefresh", TRUE) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_chkBalloon, BM_SETCHECK,
                 ConfigGetBool(L"BalloonNotify", TRUE) ? BST_CHECKED : BST_UNCHECKED, 0);
    StringCchPrintfW(buf, 16, L"%ld", ConfigGetLong(L"AutoRefreshInterval", 10));
    SetWindowTextW(s_edInterval, buf);
    SendMessageW(s_chkOrphanEn, BM_SETCHECK,
                 ConfigGetBool(L"OrphanAutoEnable", FALSE) ? BST_CHECKED : BST_UNCHECKED, 0);
    StringCchPrintfW(buf, 16, L"%ld", ConfigGetLong(L"OrphanIntervalMin", 30));
    SetWindowTextW(s_edOrphanIv, buf);
    SendMessageW(s_chkOrphanNp, BM_SETCHECK,
                 ConfigGetBool(L"OrphanNodePyOnly", TRUE) ? BST_CHECKED : BST_UNCHECKED, 0);
    {
        ThemeMode m = ThemeGetMode();
        SendMessageW(s_rdAuto, BM_SETCHECK, m == THEME_AUTO ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(s_rdLight, BM_SETCHECK, m == THEME_LIGHT ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(s_rdDark, BM_SETCHECK, m == THEME_DARK ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    s_loading = FALSE;
}

static void Layout(void)
{
    if (!s_dlg)
        return;
    int pad = SC(14), lineH = SC(28);

    MoveWindow(s_chkAutostart, pad + SC(4), SC(96), SC(340), lineH, TRUE);
    MoveWindow(s_chkStartMin, pad + SC(4), SC(96) + lineH, SC(340), lineH, TRUE);
    MoveWindow(s_rdAuto, pad + SC(4), SC(196), SC(110), lineH, TRUE);
    MoveWindow(s_rdLight, pad + SC(124), SC(196), SC(90), lineH, TRUE);
    MoveWindow(s_rdDark, pad + SC(224), SC(196), SC(90), lineH, TRUE);
    MoveWindow(s_chkAutoEn, pad + SC(4), SC(284), SC(110), lineH, TRUE);
    MoveWindow(s_edInterval, pad + SC(130), SC(284), SC(64), lineH - SC(2), TRUE);
    MoveWindow(s_udInterval, pad + SC(130) + SC(64), SC(284), SC(20), lineH - SC(2), TRUE);
    /* 孤儿进程清理区 */
    MoveWindow(s_chkOrphanEn, pad + SC(4), SC(364), SC(180), lineH, TRUE);
    MoveWindow(s_edOrphanIv, pad + SC(230), SC(364), SC(64), lineH - SC(2), TRUE);
    MoveWindow(s_udOrphanIv, pad + SC(230) + SC(64), SC(364), SC(20), lineH - SC(2), TRUE);
    MoveWindow(s_chkOrphanNp, pad + SC(4), SC(364) + lineH, SC(360), lineH, TRUE);
    MoveWindow(s_chkBalloon, pad + SC(4), SC(504), SC(360), lineH, TRUE);
    MoveWindow(s_btnClose, SC(330), SC(560), SC(90), SC(30), TRUE);
    /* AI 推荐按钮 */
    {
        HWND btnAi = GetDlgItem(s_dlg, IDC_SET_AI_RECOMMEND);
        if (btnAi)
            MoveWindow(btnAi, pad + SC(4), SC(600), SC(120), SC(30), TRUE);
    }
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
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

    case WM_TIMER:
        if (wp == 2) { /* 打开 150ms 后整体重绘：规避深色首帧竞态 */
            KillTimer(hwnd, 2);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;

    case WM_DPICHANGED: { /* 跨屏 DPI 切换：按系统建议矩形重排（本地 DPI，不污染主窗口） */
        RECT *sug = (RECT *)lp;
        s_dpi = HIWORD(wp);
        if (sug)
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        /* 系统深浅色切换直达本窗口（跟随系统模式） */
        if (ThemeOnSettingChange((LPCWSTR)lp))
            ApplyThemeSelf();
        return 0;

    case WM_COMMAND:
        if (s_loading)
            return 0;
        switch (LOWORD(wp)) {
        case IDC_SET_AUTOSTART:
            if (SendMessageW(s_chkAutostart, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                if (!StartupEnable()) {
                    MessageBoxW(hwnd, L"写入自启动注册表失败。", L"错误",
                                MB_OK | MB_ICONERROR);
                    LoadValues();
                }
            } else {
                StartupDisable();
            }
            break;
        case IDC_SET_STARTMIN:
            ConfigSetBool(L"StartMinimized",
                          SendMessageW(s_chkStartMin, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_SET_THEME_AUTO:
        case IDC_SET_THEME_LIGHT:
        case IDC_SET_THEME_DARK:
            ThemeSetMode((ThemeMode)(LOWORD(wp) - IDC_SET_THEME_AUTO));
            GuiApplySettings();       /* 主窗口热切换 */
            ApplyThemeSelf();         /* 设置窗口自身 */
            ActionsAiOnThemeChanged(); /* AI 评估窗口（若打开） */
            break;
        case IDC_SET_AUTOEN:
            ConfigSetBool(L"AutoRefresh",
                          SendMessageW(s_chkAutoEn, BM_GETCHECK, 0, 0) == BST_CHECKED);
            GuiApplySettings();
            break;
        case IDC_SET_AUTOIV:
            if (HIWORD(wp) == EN_CHANGE) {
                WCHAR buf[16];
                LONG v;
                GetWindowTextW(s_edInterval, buf, 16);
                v = _wtol(buf);
                if (v < 3)
                    v = 3;
                if (v > 3600)
                    v = 3600;
                ConfigSetLong(L"AutoRefreshInterval", v);
                GuiApplySettings();
            }
            break;
        case IDC_SET_BALLOON:
            ConfigSetBool(L"BalloonNotify",
                          SendMessageW(s_chkBalloon, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_SET_ORPHANEN:
            ConfigSetBool(L"OrphanAutoEnable",
                          SendMessageW(s_chkOrphanEn, BM_GETCHECK, 0, 0) == BST_CHECKED);
            GuiApplySettings(); /* 重挂/摘除定时器 */
            break;
        case IDC_SET_ORPHANIV:
            if (HIWORD(wp) == EN_CHANGE) {
                WCHAR obuf[16];
                LONG ov;
                GetWindowTextW(s_edOrphanIv, obuf, 16);
                ov = _wtol(obuf);
                if (ov < 1)
                    ov = 1;
                if (ov > 1440)
                    ov = 1440;
                ConfigSetLong(L"OrphanIntervalMin", ov);
                GuiApplySettings();
            }
            break;
        case IDC_SET_ORPHANNP:
            ConfigSetBool(L"OrphanNodePyOnly",
                          SendMessageW(s_chkOrphanNp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_SET_CLOSE:
            DestroyWindow(hwnd);
            break;
        case IDC_SET_AI_RECOMMEND:
            ActionsAiConfigRecommend(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (hwnd == s_dlg)
            s_dlg = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateLabel(HWND parent, const WCHAR *text, int x, int y, int w, BOOL bold)
{
    HWND h = CreateWindowExW(0, L"STATIC", text,
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             x, y, w, SC(22), parent, NULL, g_app.hInst, NULL);
    if (g_app.hFont) {
        if (bold) {
            HFONT bf = CreateFontW(-AppScale(12), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            if (bf) {
                SendMessageW(h, WM_SETFONT, (WPARAM)bf, TRUE);
                return; /* 字体由系统管理，避免复杂化不释放 */
            }
        }
        SendMessageW(h, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
    }
}

void SettingsShow(void)
{
    WNDCLASSEXW wc;

    if (s_dlg) {
        ShowWindow(s_dlg, SW_RESTORE);
        SetForegroundWindow(s_dlg);
        return;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = g_app.hInst;
    wc.hIcon = LoadIconW(g_app.hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL; /* WM_ERASEBKGND 统一绘制（dark 填充/light 默认） */
    wc.lpszClassName = SETTINGS_CLASS;
    RegisterClassExW(&wc); /* 重复注册失败可忽略 */

    /* 锚定主窗口附近：避免 CW_USEDEFAULT 漂到其他 DPI 的屏幕 */
    s_dpi = g_app.dpi;
    {
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (g_app.hMain) {
            RECT rm;
            GetWindowRect(g_app.hMain, &rm);
            x = rm.left + MulDiv(60, g_app.dpi, 96);
            y = rm.top + MulDiv(70, g_app.dpi, 96);
        }
        s_dlg = CreateWindowExW(0, SETTINGS_CLASS, L"设置",
                                WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
                                x, y, SC(440), SC(680),
                                g_app.hMain, NULL, g_app.hInst, NULL);
    }
    if (!s_dlg)
        return;

    int pad = SC(14);
    CreateLabel(s_dlg, L"▎通用", pad, SC(64), SC(200), TRUE);
    s_chkAutostart = CreateWindowExW(0, L"BUTTON", L"开机自启动（HKCU Run 注册表）",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     0, 0, SC(340), SC(28),
                                     s_dlg, (HMENU)(INT_PTR)IDC_SET_AUTOSTART, g_app.hInst, NULL);
    s_chkStartMin = CreateWindowExW(0, L"BUTTON", L"启动时最小化到托盘",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, SC(340), SC(28),
                                    s_dlg, (HMENU)(INT_PTR)IDC_SET_STARTMIN, g_app.hInst, NULL);
    CreateLabel(s_dlg, L"▎界面", pad, SC(164), SC(200), TRUE);
    s_rdAuto = CreateWindowExW(0, L"BUTTON", L"跟随系统",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                               0, 0, SC(110), SC(28),
                               s_dlg, (HMENU)(INT_PTR)IDC_SET_THEME_AUTO, g_app.hInst, NULL);
    s_rdLight = CreateWindowExW(0, L"BUTTON", L"浅色",
                                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                0, 0, SC(90), SC(28),
                                s_dlg, (HMENU)(INT_PTR)IDC_SET_THEME_LIGHT, g_app.hInst, NULL);
    s_rdDark = CreateWindowExW(0, L"BUTTON", L"深色",
                               WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                               0, 0, SC(90), SC(28),
                               s_dlg, (HMENU)(INT_PTR)IDC_SET_THEME_DARK, g_app.hInst, NULL);
    CreateLabel(s_dlg, L"▎自动刷新", pad, SC(252), SC(200), TRUE);
    s_chkAutoEn = CreateWindowExW(0, L"BUTTON", L"启用",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                  0, 0, SC(110), SC(28),
                                  s_dlg, (HMENU)(INT_PTR)IDC_SET_AUTOEN, g_app.hInst, NULL);
    s_edInterval = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                                   0, 0, SC(64), SC(26),
                                   s_dlg, (HMENU)(INT_PTR)IDC_SET_AUTOIV, g_app.hInst, NULL);
    s_udInterval = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
                                   WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT |
                                       UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                   0, 0, SC(20), SC(26),
                                   s_dlg, NULL, g_app.hInst, NULL);
    SendMessageW(s_udInterval, UDM_SETBUDDY, (WPARAM)s_edInterval, 0);
    SendMessageW(s_udInterval, UDM_SETRANGE32, 3, 3600);
    CreateLabel(s_dlg, L"秒（3~3600）", pad + SC(204), SC(286), SC(140), FALSE);
    /* 孤儿进程清理 */
    CreateLabel(s_dlg, L"▎孤儿进程清理", pad, SC(332), SC(200), TRUE);
    s_chkOrphanEn = CreateWindowExW(0, L"BUTTON", L"定时自动清理",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, SC(180), SC(28),
                                    s_dlg, (HMENU)(INT_PTR)IDC_SET_ORPHANEN, g_app.hInst, NULL);
    s_edOrphanIv = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"30",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                                   0, 0, SC(64), SC(26),
                                   s_dlg, (HMENU)(INT_PTR)IDC_SET_ORPHANIV, g_app.hInst, NULL);
    s_udOrphanIv = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
                                   WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT |
                                       UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                   0, 0, SC(20), SC(26),
                                   s_dlg, NULL, g_app.hInst, NULL);
    SendMessageW(s_udOrphanIv, UDM_SETBUDDY, (WPARAM)s_edOrphanIv, 0);
    SendMessageW(s_udOrphanIv, UDM_SETRANGE32, 1, 1440);
    CreateLabel(s_dlg, L"分钟（1~1440）", pad + SC(304), SC(366), SC(130), FALSE);
    s_chkOrphanNp = CreateWindowExW(0, L"BUTTON", L"仅清理 Node/Python 进程（推荐）",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, SC(360), SC(28),
                                    s_dlg, (HMENU)(INT_PTR)IDC_SET_ORPHANNP, g_app.hInst, NULL);
    CreateLabel(s_dlg, L"▎通知", pad, SC(472), SC(200), TRUE);
    s_chkBalloon = CreateWindowExW(0, L"BUTTON", L"显示气泡通知（清理/复制/自启等结果提示）",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   0, 0, SC(360), SC(28),
                                   s_dlg, (HMENU)(INT_PTR)IDC_SET_BALLOON, g_app.hInst, NULL);
    s_btnClose = CreateWindowExW(0, L"BUTTON", L"关闭",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, SC(90), SC(30),
                                 s_dlg, (HMENU)(INT_PTR)IDC_SET_CLOSE, g_app.hInst, NULL);
    CreateLabel(s_dlg, L"▎AI", pad, SC(570), SC(200), TRUE);
    {
        HWND btnAi = CreateWindowExW(0, L"BUTTON", L"AI 推荐配置",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, SC(120), SC(30),
                                     s_dlg, (HMENU)(INT_PTR)IDC_SET_AI_RECOMMEND,
                                     g_app.hInst, NULL);
        if (g_app.hFont)
            SendMessageW(btnAi, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
    }

    HWND ctrls[] = { s_chkAutostart, s_chkStartMin, s_rdAuto, s_rdLight, s_rdDark,
                     s_chkAutoEn, s_edInterval, s_chkBalloon, s_btnClose,
                     s_chkOrphanEn, s_edOrphanIv, s_chkOrphanNp };
    for (size_t i = 0; i < sizeof(ctrls) / sizeof(ctrls[0]); i++)
        if (ctrls[i] && g_app.hFont)
            SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_app.hFont, TRUE);

    Layout();
    LoadValues();
    ApplyThemeSelf();

    ShowWindow(s_dlg, SW_SHOW);
    UpdateWindow(s_dlg);
    /* 二次失效规避深色首帧竞态 */
    SetTimer(s_dlg, 2, 150, NULL);
    return;
}

/* 设置窗口 2 号定时器：打开 150ms 后整体重绘一次（规避深色首帧竞态） */
/* 处理位于 SettingsProc WM_TIMER */

void SettingsOnThemeChanged(void)
{
    if (!s_dlg)
        return;
    ApplyThemeSelf();
}

BOOL SettingsIsVisible(void)
{
    return s_dlg != NULL;
}
