/* project.c - 项目根目录识别实现（WP8）
 * 纯本地文件系统探测，不依赖 AI。
 */
#include "common.h"
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "peb.h"
#include "project.h"

static WCHAR g_rootBuf[MAX_PATH];

/* 检查目录下是否存在任一项目标识文件 */
static BOOL HasProjectMarker(const WCHAR *dir)
{
    static const WCHAR *const markers[] = {
        L"\\package.json",
        L"\\pyproject.toml",
        L"\\requirements.txt",
    };

    for (int i = 0; i < 3; i++) {
        WCHAR path[MAX_PATH];
        StringCchPrintfW(path, MAX_PATH, L"%ls%ls", dir, markers[i]);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            return TRUE;
    }
    return FALSE;
}

const WCHAR *FindProjectRoot(const WCHAR *cwd)
{
    WCHAR dir[MAX_PATH];
    int levels;

    if (!cwd || !cwd[0])
        return NULL;
    StringCchCopyW(dir, MAX_PATH, cwd);
    /* 去尾部反斜杠（保留根目录如 C:\） */
    {
        size_t len = lstrlenW(dir);
        while (len > 3 && dir[len - 1] == L'\\') {
            dir[len - 1] = L'\0';
            len--;
        }
    }

    for (levels = 0; levels < 4; levels++) { /* cwd 自身 + 向上 3 级 */
        if (HasProjectMarker(dir)) {
            StringCchCopyW(g_rootBuf, MAX_PATH, dir);
            return g_rootBuf;
        }
        /* 向上一级 */
        {
            WCHAR *slash = wcsrchr(dir, L'\\');
            if (!slash || slash == dir)
                break; /* 已到根 */
            *slash = L'\0';
            if (dir[lstrlenW(dir) - 1] == L':')
                break; /* C: 根目录 */
        }
    }
    return NULL;
}

BOOL GetProcessProject(DWORD pid, WCHAR *out, size_t cch)
{
    WCHAR cwd[MAX_PATH];

    if (!out || cch == 0)
        return FALSE;
    out[0] = L'\0';
    if (!PebQuery(pid, NULL, 0, cwd, MAX_PATH) || !cwd[0])
        return FALSE;
    {
        const WCHAR *root = FindProjectRoot(cwd);
        if (root) {
            StringCchCopyW(out, cch, root);
            return TRUE;
        }
    }
    return FALSE;
}
