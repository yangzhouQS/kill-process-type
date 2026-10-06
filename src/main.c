/* main.c - 程序入口：无头 CLI、配置/日志/主题初始化、单实例互斥、/tray 参数、消息循环 */
#include "common.h"
#include <commctrl.h>
#include <strsafe.h>
#include <wchar.h>

#include "cli.h"
#include "config.h"
#include "klog.h"
#include "monitor.h"
#include "theme.h"
#include "gui.h"

#define SINGLE_INSTANCE_MUTEX L"Local\\kill-process-type-single"

/* 未处理异常捕获：写入 exe 旁 crash.txt（异常码+地址），便于远程定位静默退出 */
static LONG WINAPI CrashTrap(EXCEPTION_POINTERS *ep)
{
    WCHAR path[MAX_PATH], exe[MAX_PATH], msg[256];
    HANDLE h;

    if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        WCHAR *slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = 0;
            StringCchPrintfW(path, MAX_PATH, L"%ls\\crash.txt", exe);
            h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                StringCchPrintfW(msg, 256,
                    L"code=0x%08lX addr=0x%p\n",
                    (unsigned long)ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress);
                {
                    char a[256];
                    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, a, 256, NULL, NULL);
                    DWORD w;
                    if (n > 0)
                        WriteFile(h, a, n - 1, &w, NULL);
                }
                CloseHandle(h);
            }
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR pCmdLine, int nCmdShow)
{
    (void)hPrev;

    SetUnhandledExceptionFilter(CrashTrap);

    /* /list /ports /orphans /kill：无头 CLI 模式（供 kilo 等 AI 工具链集成），输出后退出 */
    if (CliDispatch(pCmdLine))
        return 0;

    /* 配置/日志/主题必须在窗口创建前就绪 */
    ConfigInit();
    KlogInit();
    ThemeInit();

    /* /tray 或 -tray：静默启动到托盘（自启动带气泡提示） */
    BOOL startHidden = pCmdLine != NULL &&
                       (wcsstr(pCmdLine, L"/tray") || wcsstr(pCmdLine, L"-tray"));
    if (startHidden)
        nCmdShow = SW_HIDE;
    else if (ConfigGetBool(L"StartMinimized", FALSE))
        nCmdShow = SW_SHOWMINNOACTIVE; /* 启动时最小化（静默，无气泡） */

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
    MonitorStart(); /* WP11: 时序监控线程 */

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex)
        CloseHandle(hMutex);
    return (int)msg.wParam;
}
