/* tray.c - 托盘图标、托盘右键菜单、气泡通知 */
#include "common.h"
#include <shellapi.h>
#include <strsafe.h>

#include "config.h"
#include "resource.h"
#include "startup.h"
#include "tray.h"

static NOTIFYICONDATAW s_nid;
static BOOL s_added = FALSE;

void TrayInit(HWND owner, HINSTANCE hInst, UINT callbackMsg)
{
    ZeroMemory(&s_nid, sizeof(s_nid));
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = owner;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    s_nid.uCallbackMessage = callbackMsg;
    s_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!s_nid.hIcon)
        s_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    StringCchCopyW(s_nid.szTip, sizeof(s_nid.szTip) / sizeof(s_nid.szTip[0]),
                   L"Node/Python 进程终结者");
}

BOOL TrayAdd(void)
{
    s_added = Shell_NotifyIconW(NIM_ADD, &s_nid);
    return s_added;
}

void TrayRemove(void)
{
    if (s_added) {
        Shell_NotifyIconW(NIM_DELETE, &s_nid);
        s_added = FALSE;
    }
}

void TrayShowBalloon(const WCHAR *title, const WCHAR *text)
{
    if (!s_added)
        return;
    if (!ConfigGetBool(L"BalloonNotify", TRUE)) /* 设置：关闭气泡通知 */
        return;
    NOTIFYICONDATAW n = s_nid;
    n.uFlags |= NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    StringCchCopyW(n.szInfoTitle, sizeof(n.szInfoTitle) / sizeof(n.szInfoTitle[0]), title);
    StringCchCopyW(n.szInfo, sizeof(n.szInfo) / sizeof(n.szInfo[0]), text);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayToggleWindow(HWND owner)
{
    if (IsWindowVisible(owner)) {
        ShowWindow(owner, SW_HIDE);
    } else {
        if (IsIconic(owner))
            ShowWindow(owner, SW_RESTORE);
        else
            ShowWindow(owner, SW_SHOW);
        SetForegroundWindow(owner);
    }
}

LRESULT TrayHandleMessage(HWND owner, WPARAM wp, LPARAM lp)
{
    if ((UINT)wp != s_nid.uID)
        return 0;

    if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU m = CreatePopupMenu();
        if (m) {
            AppendMenuW(m, MF_STRING, IDM_TRAY_REFRESH, L"刷新进程列表");
            AppendMenuW(m, MF_STRING, IDM_TRAY_KILL_NODE, L"杀死全部 Node.js");
            AppendMenuW(m, MF_STRING, IDM_TRAY_KILL_PY, L"杀死全部 Python");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING | (StartupIsEnabled() ? MF_CHECKED : 0),
                        IDM_TRAY_AUTOSTART, L"开机自启动");
            AppendMenuW(m, MF_STRING, IDM_TRAY_SETTINGS, L"设置...");
            AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"显示 / 隐藏主界面");
            AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"退出");
            SetForegroundWindow(owner);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, NULL);
            PostMessageW(owner, WM_NULL, 0, 0);
            DestroyMenu(m);
        }
    } else if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
        TrayToggleWindow(owner);
    }
    return 0;
}
