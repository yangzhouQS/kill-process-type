/* startup.c - 开机自启动实现：HKCU\Software\Microsoft\Windows\CurrentVersion\Run
 * 值名 KillProcessType，数据为带引号的 exe 完整路径 + " /tray" 参数，
 * 登录后由 main.c 解析 /tray 并静默驻留托盘。
 */
#include "common.h"
#include <strsafe.h>

#include "startup.h"

static const WCHAR RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR RUN_VALUE[] = L"KillProcessType";

BOOL StartupIsEnabled(void)
{
    HKEY k;
    DWORD type = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    LONG r = RegQueryValueExW(k, RUN_VALUE, NULL, &type, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

BOOL StartupEnable(void)
{
    WCHAR exe[MAX_PATH];
    WCHAR cmd[MAX_PATH + 16];
    HKEY k;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return FALSE;
    StringCchPrintfW(cmd, MAX_PATH + 16, L"\"%ls\" /tray", exe);

    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    LONG r = RegSetValueExW(k, RUN_VALUE, 0, REG_SZ,
                            (const BYTE *)cmd,
                            (DWORD)((lstrlenW(cmd) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

BOOL StartupDisable(void)
{
    HKEY k;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    LONG r = RegDeleteValueW(k, RUN_VALUE);
    RegCloseKey(k);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}
