/* theme.c - 深色主题实现：
 * - 标题栏：DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)
 * - 列表/表头/状态栏/编辑框：消息着色 + 表头 NM_CUSTOMDRAW 自绘
 * - Tab 控件：子类化擦除背景 + TCS_OWNERDRAWFIXED 自绘项
 * - 按钮：BS_OWNERDRAW 自绘深色（checkbox 走 WM_CTLCOLORBTN）
 * 浅色模式一律走系统默认，零侵入。
 */
#include "common.h"
#include <commctrl.h>
#include <strsafe.h>
#include <wchar.h>

#include "config.h"
#include "theme.h"

#define CLR_BACK    RGB(32, 32, 36)
#define CLR_ALT     RGB(48, 48, 54)
#define CLR_TEXT    RGB(235, 235, 235)
#define CLR_ACCENT  RGB(0, 120, 212)

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static ThemeMode s_mode = THEME_AUTO;
static BOOL s_dark = FALSE;
static HBRUSH s_brBack;
static HBRUSH s_brAlt;

static BOOL QuerySystemDark(void)
{
    DWORD light = 1, size = sizeof(DWORD);
    HKEY k;

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AppsUseLightTheme", NULL, NULL,
                             (LPBYTE)&light, &size) != ERROR_SUCCESS)
            light = 1;
        RegCloseKey(k);
    }
    return light == 0;
}

static void ThemeRecalc(void)
{
    s_dark = (s_mode == THEME_DARK) ||
             (s_mode == THEME_AUTO && QuerySystemDark());
}

static void EnsureBrushes(void)
{
    if (!s_brBack)
        s_brBack = CreateSolidBrush(CLR_BACK);
    if (!s_brAlt)
        s_brAlt = CreateSolidBrush(CLR_ALT);
}

void ThemeInit(void)
{
    LONG m = ConfigGetLong(L"Theme", 0);

    if (m < 0 || m > 2)
        m = 0;
    s_mode = (ThemeMode)m;
    ThemeRecalc();
    EnsureBrushes();
}

ThemeMode ThemeGetMode(void) { return s_mode; }

void ThemeSetMode(ThemeMode mode)
{
    s_mode = mode;
    ConfigSetLong(L"Theme", (LONG)mode);
    ThemeRecalc();
    EnsureBrushes();
}

BOOL ThemeIsDark(void) { return s_dark; }
COLORREF ThemeGetBackColor(void) { return CLR_BACK; }
COLORREF ThemeGetTextColor(void) { return CLR_TEXT; }
HBRUSH ThemeGetBackBrush(void) { EnsureBrushes(); return s_brBack; }

/* ---------- 控件应用 ---------- */

void ThemeApplyFrame(HWND hwnd)
{
    typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    BOOL dark;
    PFN_DwmSetWindowAttribute p;

    if (!dwm)
        return;
    p = (PFN_DwmSetWindowAttribute)(void *)(INT_PTR)
        GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (p) {
        dark = s_dark;
        /* 20 为 20H1+，19 为旧版 1809 */
        if (FAILED(p(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
            p(hwnd, 19, &dark, sizeof(dark));
    }
    FreeLibrary(dwm);
}

void ThemeApplyListView(HWND lv)
{
    if (!lv)
        return;
    if (s_dark) {
        ListView_SetBkColor(lv, CLR_BACK);
        ListView_SetTextBkColor(lv, CLR_BACK);
        ListView_SetTextColor(lv, CLR_TEXT);
    } else {
        ListView_SetBkColor(lv, CLR_NONE);
        ListView_SetTextBkColor(lv, CLR_NONE);
        ListView_SetTextColor(lv, GetSysColor(COLOR_WINDOWTEXT));
    }
    InvalidateRect(lv, NULL, TRUE);
}

void ThemeApplyStatusBar(HWND sb)
{
    if (!sb)
        return;
    if (s_dark)
        SendMessageW(sb, SB_SETBKCOLOR, 0, (LPARAM)CLR_BACK);
    else
        SendMessageW(sb, SB_SETBKCOLOR, 0, (LPARAM)CLR_DEFAULT);
    InvalidateRect(sb, NULL, TRUE);
}

void ThemeApplyEdit(HWND edit)
{
    if (!edit)
        return;
    InvalidateRect(edit, NULL, TRUE);
}

void ThemeApplyButtons(HWND parent, const INT *ids, int count)
{
    for (int i = 0; i < count && ids; i++) {
        HWND btn = GetDlgItem(parent, ids[i]);
        DWORD style;

        if (!btn)
            continue;
        style = (DWORD)GetWindowLongPtrW(btn, GWL_STYLE);
        if (s_dark)
            style |= BS_OWNERDRAW;
        else
            style &= ~(DWORD)BS_OWNERDRAW;
        SetWindowLongPtrW(btn, GWL_STYLE, (LONG_PTR)style);
        SetWindowPos(btn, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        InvalidateRect(btn, NULL, TRUE);
        UpdateWindow(btn); /* 同步立即重绘，规避 ownerdraw 首帧缺失 */
    }
}

/* ---------- Tab 子类化 ---------- */

static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    (void)dwRefData;
    if (msg == WM_ERASEBKGND && s_dark) {
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        EnsureBrushes();
        FillRect(dc, &rc, s_brBack);
        return 1;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TabSubclassProc, 1);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void ThemeApplyTabControl(HWND tab)
{
    DWORD style;

    if (!tab)
        return;
    SetWindowSubclass(tab, TabSubclassProc, 1, 0);
    style = (DWORD)GetWindowLongPtrW(tab, GWL_STYLE);
    if (s_dark)
        style |= TCS_OWNERDRAWFIXED;
    else
        style &= ~(DWORD)TCS_OWNERDRAWFIXED;
    SetWindowLongPtrW(tab, GWL_STYLE, (LONG_PTR)style);
    InvalidateRect(tab, NULL, TRUE);
}

/* ---------- 消息路由 ---------- */

BOOL ThemeOnEraseBkgnd(HWND hwnd, HDC hdc)
{
    RECT rc;

    GetClientRect(hwnd, &rc);
    if (s_dark) {
        EnsureBrushes();
        FillRect(hdc, &rc, s_brBack);
    } else {
        /* 浅色也显式填充：窗口类刷子为 NULL 的动态窗口（设置/AI）必须擦除，
         * 否则深->浅切换后残留旧深色背景 */
        FillRect(hdc, &rc, (HBRUSH)GetSysColorBrush(COLOR_WINDOW));
    }
    return TRUE;
}

HBRUSH ThemeOnCtlColor(HWND ctrl, HDC hdc)
{
    WCHAR cls[32];

    if (!s_dark || !ctrl)
        return NULL;
    if (!GetClassNameW(ctrl, cls, 32))
        return NULL;
    if (wcscmp(cls, L"Edit") == 0) {
        SetBkColor(hdc, CLR_BACK);
        SetTextColor(hdc, CLR_TEXT);
        EnsureBrushes();
        return s_brBack;
    }
    if (wcscmp(cls, L"Static") == 0 || wcscmp(cls, L"Button") == 0 ||
        wcscmp(cls, L"msctls_statusbar32") == 0) {
        SetBkColor(hdc, CLR_BACK);
        SetTextColor(hdc, CLR_TEXT);
        EnsureBrushes();
        return s_brBack;
    }
    return NULL;
}

BOOL ThemeOnDrawItem(LPARAM lp)
{
    DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;

    if (!s_dark || !dis)
        return FALSE;

    if (dis->CtlType == ODT_BUTTON) {
        RECT rc = dis->rcItem;
        HBRUSH br;
        HBRUSH selBrush = NULL;

        EnsureBrushes();
        if (dis->itemState & ODS_SELECTED) {
            selBrush = CreateSolidBrush(CLR_ACCENT);
            br = selBrush;
        } else {
            br = s_brAlt;
        }
        FillRect(dis->hDC, &rc, br);
        if (selBrush)
            DeleteObject(selBrush);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, CLR_TEXT);
        {
            WCHAR text[128];
            if (GetWindowTextW(dis->hwndItem, text, 128))
                DrawTextW(dis->hDC, text, -1, &rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (dis->itemState & ODS_FOCUS) {
            RECT fr = rc;
            InflateRect(&fr, -3, -3);
            DrawFocusRect(dis->hDC, &fr);
        }
        return TRUE;
    }

    if (dis->CtlType == ODT_TAB) {
        RECT rc = dis->rcItem;
        TCITEMW ti;
        WCHAR text[64];
        BOOL sel = (dis->itemState & ODS_SELECTED) != 0;

        EnsureBrushes();
        FillRect(dis->hDC, &rc, sel ? s_brAlt : s_brBack);
        ZeroMemory(&ti, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = text;
        ti.cchTextMax = 64;
        if (SendMessageW(dis->hwndItem, TCM_GETITEMW, dis->itemID, (LPARAM)&ti)) {
            HFONT prev = (HFONT)SelectObject(
                dis->hDC, (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, sel ? CLR_TEXT : RGB(170, 170, 175));
            DrawTextW(dis->hDC, text, -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, prev);
        }
        return TRUE;
    }
    return FALSE;
}

BOOL ThemeOnHeaderNotify(LPNMHDR hdr, LRESULT *result)
{
    WCHAR cls[32];

    if (!s_dark || !hdr || hdr->code != NM_CUSTOMDRAW)
        return FALSE;
    /* 只处理表头（SysHeader32）自绘：ListView 自身也会发 NM_CUSTOMDRAW，
     * 误劫持会导致列表项 CDRF_SKIPDEFAULT 而全部不绘制（空白列表） */
    if (!GetClassNameW(hdr->hwndFrom, cls, 32) || wcscmp(cls, L"SysHeader32") != 0)
        return FALSE;
    {
        NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)hdr;

        if (cd->dwDrawStage == CDDS_PREPAINT) {
            *result = CDRF_NOTIFYITEMDRAW;
            return TRUE;
        }
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
            /* 表头项完全自绘：深色底 + 文字 + 排序箭头 */
            HDITEMW hdi;
            WCHAR text[64];
            RECT rc = cd->rc;
            RECT tr;
            HFONT prevFont;

            EnsureBrushes();
            FillRect(cd->hdc, &rc, s_brBack);
            ZeroMemory(&hdi, sizeof(hdi));
            hdi.mask = HDI_TEXT | HDI_FORMAT;
            hdi.pszText = text;
            hdi.cchTextMax = 64;
            if (Header_GetItem(hdr->hwndFrom, (INT)cd->dwItemSpec, &hdi) && text[0]) {
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, CLR_TEXT);
                tr = rc;
                tr.left += 8;
                tr.right -= 18; /* 留排序箭头位 */
                DrawTextW(cd->hdc, text, -1, &tr,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            }
            if (hdi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
                WCHAR arrow[2] = { (hdi.fmt & HDF_SORTUP) ? 0x25B2 : 0x25BC, 0 };
                prevFont = (HFONT)SelectObject(
                    cd->hdc, (HFONT)SendMessageW(hdr->hwndFrom, WM_GETFONT, 0, 0));
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, RGB(140, 140, 148));
                tr = rc;
                tr.right -= 4;
                tr.left = tr.right - 14;
                DrawTextW(cd->hdc, arrow, -1, &tr,
                          DT_SINGLELINE | DT_VCENTER | DT_CENTER);
                SelectObject(cd->hdc, prevFont);
            }
            *result = CDRF_SKIPDEFAULT;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL ThemeOnSettingChange(const WCHAR *what)
{
    BOOL wasDark;

    if (s_mode != THEME_AUTO || !what)
        return FALSE;
    if (wcscmp(what, L"ImmersiveColorSet") != 0)
        return FALSE;
    wasDark = s_dark;
    ThemeRecalc();
    return s_dark != wasDark;
}
