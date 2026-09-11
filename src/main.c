/* main.c - 绋嬪簭鍏ュ彛锛氭棤澶?CLI銆侀厤缃?涓婚鍒濆鍖栥€佸崟瀹炰緥浜掓枼銆?tray 鍙傛暟銆佹秷鎭惊鐜?*/
#include "common.h"
#include <commctrl.h>
#include <wchar.h>

#include "cli.h"
#include "config.h"
#include "theme.h"
#include "gui.h"

#define SINGLE_INSTANCE_MUTEX L"Local\\kill-process-type-single"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR pCmdLine, int nCmdShow)
{
    (void)hPrev;

    /* /list /ports /kill锛氭棤澶?CLI 妯″紡锛堜緵 kilo 绛?AI 宸ュ叿閾鹃泦鎴愶級锛岃緭鍑哄悗閫€鍑?*/
    if (CliDispatch(pCmdLine))
        return 0;

    /* 閰嶇疆涓庝富棰樺繀椤诲湪绐楀彛鍒涘缓鍓嶅氨缁?*/
    ConfigInit();
    ThemeInit();

    /* /tray 鎴?-tray锛氶潤榛樺惎鍔ㄥ埌鎵樼洏锛堣嚜鍚姩甯︽皵娉℃彁绀猴級 */
    BOOL startHidden = pCmdLine != NULL &&
                       (wcsstr(pCmdLine, L"/tray") || wcsstr(pCmdLine, L"-tray"));
    if (startHidden)
        nCmdShow = SW_HIDE;
    else if (ConfigGetBool(L"StartMinimized", FALSE))
        nCmdShow = SW_SHOWMINNOACTIVE; /* 鍚姩鏃舵渶灏忓寲锛堥潤榛橈紝鏃犳皵娉★級 */

    HANDLE hMutex = CreateMutexW(NULL, FALSE, SINGLE_INSTANCE_MUTEX);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND h = FindWindowW(MAIN_WINDOW_CLASS, NULL);
        if (h) {
            if (IsIconic(h))
                ShowWindow(h, SW_RESTORE);
            else
                ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
        }
        CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    if (!GuiCreateMain(hInst, nCmdShow))
        return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex)
        CloseHandle(hMutex);
    return (int)msg.wParam;
}

