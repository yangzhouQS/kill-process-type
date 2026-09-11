/* test_startup.c - 开机自启动模块验证（结束前完整还原原值内容，不留注册表残留） */
#include "../src/common.h"
#include <stdio.h>
#include <strsafe.h>
#include "../src/startup.h"

static const WCHAR RUN_KEY_PATH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR RUN_VALUE_NAME[] = L"KillProcessType";

/* 读取当前 Run 值数据；返回值是否存在 */
static BOOL ReadRunValue(WCHAR *buf, size_t bufCch)
{
    HKEY k;
    DWORD size = (DWORD)(bufCch * sizeof(WCHAR));
    LONG r;

    buf[0] = L'\0';
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    r = RegQueryValueExW(k, RUN_VALUE_NAME, NULL, NULL, (BYTE *)buf, &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

/* 将原始数据原样写回 Run 值 */
static BOOL WriteRunValue(const WCHAR *data)
{
    HKEY k;
    LONG r;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    r = RegSetValueExW(k, RUN_VALUE_NAME, 0, REG_SZ,
                       (const BYTE *)data,
                       (DWORD)((lstrlenW(data) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

int wmain(void)
{
    WCHAR orig[MAX_PATH + 16];
    BOOL hadValue = ReadRunValue(orig, MAX_PATH + 16);

    wprintf(L"initial enabled: %d\n", (int)StartupIsEnabled());
    if (hadValue)
        wprintf(L"initial value  : %ls\n", orig);

    wprintf(L"enable  -> %d\n", (int)StartupEnable());
    wprintf(L"after enable: %d\n", (int)StartupIsEnabled());

    /* 完整还原：原值存在则写回原数据，原本不存在则删除 */
    if (hadValue) {
        wprintf(L"restore -> %d\n", (int)WriteRunValue(orig));
        wprintf(L"final value  : %ls\n", orig);
    } else {
        wprintf(L"disable -> %d\n", (int)StartupDisable());
    }
    wprintf(L"final enabled: %d (expect %d)\n", (int)StartupIsEnabled(), (int)hadValue);
    return 0;
}
