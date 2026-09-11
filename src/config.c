/* config.c - 配置持久化实现：直接使用 Win32 Profile API，免解析免缓存 */
#include "common.h"
#include <strsafe.h>
#include <wchar.h>

#include "config.h"

#define CONFIG_SECTION L"main"
#define CONFIG_NAME L"kill-process-type.ini"

static WCHAR s_path[MAX_PATH];

static BOOL CanWriteFile(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    CloseHandle(h);
    return TRUE;
}

void ConfigInit(void)
{
    WCHAR exe[MAX_PATH];
    WCHAR appData[MAX_PATH];
    WCHAR *slash;

    /* 首选：exe 同目录（便携模式） */
    if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = L'\0';
            StringCchPrintfW(s_path, MAX_PATH, L"%ls\\%ls", exe, CONFIG_NAME);
            if (CanWriteFile(s_path))
                return;
        }
    }

    /* 回退：%APPDATA% */
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH))
        StringCchPrintfW(s_path, MAX_PATH, L"%ls\\%ls", appData, CONFIG_NAME);
    else
        StringCchCopyW(s_path, MAX_PATH, CONFIG_NAME);
}

LONG ConfigGetLong(const WCHAR *key, LONG def)
{
    if (!s_path[0])
        return def;
    return (LONG)GetPrivateProfileIntW(CONFIG_SECTION, key, (UINT)def, s_path);
}

BOOL ConfigGetBool(const WCHAR *key, BOOL def)
{
    return ConfigGetLong(key, def ? 1 : 0) != 0;
}

void ConfigSetLong(const WCHAR *key, LONG val)
{
    WCHAR buf[24];

    if (!s_path[0])
        return;
    StringCchPrintfW(buf, 24, L"%ld", val);
    WritePrivateProfileStringW(CONFIG_SECTION, key, buf, s_path);
}

void ConfigSetBool(const WCHAR *key, BOOL val)
{
    ConfigSetLong(key, val ? 1 : 0);
}

const WCHAR *ConfigGetPath(void)
{
    return s_path;
}
