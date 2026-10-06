/* klog.c - 终止进程日志实现：
 * 文件与配置同目录（便携时在 exe 旁，否则 %APPDATA%），UTF-8 BOM，
 * 制表符分隔字段，超 512KB 滚动为 .old 重新开档。
 */
#include "common.h"
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "config.h"
#include "klog.h"

#if defined(_MSC_VER)
#else
/* mingw 的 _wtoll 在部分头文件配置下不可见，用 wcstoll(n,NULL,10) 替代 */
static LONGLONG WToLL(const WCHAR *s) { return wcstoll(s, NULL, 10); }
#define _wtoll(s) WToLL(s)
#endif

static WCHAR s_path[MAX_PATH];
static WCHAR s_dir[MAX_PATH];

/* 记录格式：unix \t 时间 \t 来源 \t 名称 \t PID \t 结果 \t 错误码 \t 路径 */

void KlogInit(void)
{
    const WCHAR *ini = ConfigGetPath();
    const WCHAR *slash;

    if (!ini || !ini[0]) {
        StringCchCopyW(s_dir, MAX_PATH, L".");
        StringCchCopyW(s_path, MAX_PATH, L"kill-process-type.log");
        return;
    }
    slash = wcsrchr(ini, L'\\');
    if (slash) {
        size_t n = (size_t)(slash - ini);
        if (n >= MAX_PATH)
            n = MAX_PATH - 1;
        StringCchCopyNW(s_dir, MAX_PATH, ini, n + 1);
        s_dir[n] = L'\0';
        StringCchPrintfW(s_path, MAX_PATH, L"%ls\\kill-process-type.log", s_dir);
    } else {
        StringCchCopyW(s_dir, MAX_PATH, L".");
        StringCchCopyW(s_path, MAX_PATH, L"kill-process-type.log");
    }

    /* 超限滚动：log -> log.old（覆盖旧 .old） */
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(s_path, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeLow > KLOG_MAX_BYTES) {
            WCHAR oldPath[MAX_PATH];
            StringCchPrintfW(oldPath, MAX_PATH, L"%ls.old", s_path);
            DeleteFileW(oldPath);
            MoveFileW(s_path, oldPath);
        }
    }
}

const WCHAR *KlogGetPath(void)
{
    return s_path;
}

void KlogWrite(const WCHAR *source, const ProcInfo *info, DWORD pid,
               BOOL ok, DWORD err)
{
    static const WCHAR *const kMonth[] = {
        L"01", L"02", L"03", L"04", L"05", L"06",
        L"07", L"08", L"09", L"10", L"11", L"12"
    };
    HANDLE h;
    DWORD written;
    SYSTEMTIME st;
    WCHAR line[64 + 24 + 16 + 64 + 16 + 16 + 12 + MAX_PATH];
    char utf8[2048];
    int n;

    if (!s_path[0])
        return;
    GetLocalTime(&st);
    if (st.wMonth < 1 || st.wMonth > 12)
        st.wMonth = 1;
    StringCchPrintfW(line, sizeof(line) / sizeof(WCHAR),
                     L"%lu%02lu%02lu%02lu%02lu%02lu\t%04u-%ls-%02u %02u:%02u:%02u\t%ls\t%ls\t%lu\t%ls\t%lu\t%ls\r\n",
                     (unsigned long)st.wYear, (unsigned long)st.wMonth,
                     (unsigned long)st.wDay, (unsigned long)st.wHour,
                     (unsigned long)st.wMinute, (unsigned long)st.wSecond,
                     st.wYear, kMonth[st.wMonth - 1], st.wDay,
                     st.wHour, st.wMinute, st.wSecond,
                     source ? source : L"-",
                     info ? info->name : L"(未知)",
                     (unsigned long)pid,
                     ok ? L"已终止" : L"失败",
                     (unsigned long)err,
                     (info && info->path[0]) ? info->path : L"-");

    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 1)
        return;
    n--; /* 去掉结尾 NUL */

    h = CreateFileW(s_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (GetFileSize(h, NULL) == 0) {
        static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
        WriteFile(h, bom, 3, &written, NULL);
    }
    WriteFile(h, utf8, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

/* 解析一行（\t 分隔 8 字段）到 entry；返回是否成功 */
static BOOL ParseLogLine(const WCHAR *line, LogEntry *e)
{
    const WCHAR *p = line;
    WCHAR num[24];

    ZeroMemory(e, sizeof(*e));
    /* 字段 0：unix 时间戳（无分隔符数字） */
    {
        int i = 0;
        while (*p >= L'0' && *p <= L'9' && i < 18)
            num[i++] = *p++;
        num[i] = L'\0';
        e->unixTime = _wtoll(num);
    }
    if (*p++ != L'\t')
        return FALSE;
    /* 字段 1：时间文本 */
    {
        int i = 0;
        while (*p && *p != L'\t' && i < 23)
            e->timeText[i++] = *p++;
        e->timeText[i] = L'\0';
    }
    if (*p++ != L'\t')
        return FALSE;
    /* 字段 2：来源 */
    {
        int i = 0;
        while (*p && *p != L'\t' && i < 15)
            e->source[i++] = *p++;
        e->source[i] = L'\0';
    }
    if (*p++ != L'\t')
        return FALSE;
    /* 字段 3：名称 */
    {
        int i = 0;
        while (*p && *p != L'\t' && i < 63)
            e->name[i++] = *p++;
        e->name[i] = L'\0';
    }
    if (*p++ != L'\t')
        return FALSE;
    /* 字段 4：PID */
    {
        int i = 0;
        while (*p >= L'0' && *p <= L'9' && i < 12)
            num[i++] = *p++;
        num[i] = L'\0';
        e->pid = (DWORD)_wtol(num);
    }
    if (*p++ != L'\t')
        return FALSE;
    /* 字段 5：结果 */
    e->ok = (*p == L'已');
    while (*p && *p != L'\t')
        p++;
    if (*p++ != L'\t')
        return FALSE;
    /* 字段 6：错误码（跳过） */
    while (*p && *p != L'\t')
        p++;
    if (*p++ != L'\t')
        return FALSE;
    /* 字段 7：路径 */
    {
        int i = 0;
        while (*p && *p != L'\r' && *p != L'\n' && i < MAX_PATH - 1)
            e->path[i++] = *p++;
        e->path[i] = L'\0';
    }
    return e->name[0] != L'\0';
}

int KlogLoad(LogList *out)
{
    HANDLE h;
    char *raw;
    DWORD size, got = 0;
    WCHAR *wide;

    ZeroMemory(out, sizeof(*out));
    if (!s_path[0])
        return -1;
    h = CreateFileW(s_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0; /* 无日志文件 = 空记录 */
    size = GetFileSize(h, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE) {
        CloseHandle(h);
        return 0;
    }
    if (size > 4 * 1024 * 1024)
        size = 4 * 1024 * 1024; /* 最多读 4MB */
    raw = (char *)malloc(size + 2);
    if (!raw) {
        CloseHandle(h);
        return -1;
    }
    if (!ReadFile(h, raw, size, &got, NULL) || got == 0) {
        free(raw);
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    raw[got] = raw[got + 1] = '\0';
    if (got >= 3 && (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF)
        got -= 3, memmove(raw, raw + 3, got); /* 去 BOM */

    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, NULL, 0);
        int total, skip;
        WCHAR *line;
        int idx = 0;
        int lineNo = 0;

        if (wlen <= 0) {
            free(raw);
            return 0;
        }
        wide = (WCHAR *)malloc(((size_t)wlen + 2) * sizeof(WCHAR));
        if (!wide) {
            free(raw);
            return -1;
        }
        MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, wide, wlen);
        wide[wlen] = L'\n';
        wide[wlen + 1] = L'\0';
        free(raw);

        total = 0;
        for (int i = 0; i < wlen; i++)
            if (wide[i] == L'\n')
                total++;
        skip = total > KLOG_MAX_ENTRIES ? total - KLOG_MAX_ENTRIES : 0;

        out->items = (LogEntry *)malloc(
            ((size_t)(total > KLOG_MAX_ENTRIES ? KLOG_MAX_ENTRIES : total) + 1) *
            sizeof(LogEntry));
        if (!out->items) {
            free(wide);
            return -1;
        }

        line = wide;
        for (WCHAR *p = wide; ; p++) {
            if (*p == L'\n' || !*p) {
                lineNo++;
                if (lineNo > skip && p > line) {
                    WCHAR term = *p;
                    *p = L'\0';
                    {
                        LogEntry e;
                        if (ParseLogLine(line, &e)) {
                            out->items[idx++] = e; /* 文件序追加 */
                        }
                    }
                    *p = term;
                }
                if (!*p)
                    break;
                line = p + 1;
            }
        }
        /* 反转为新记录在前（避免逐条头部搬移的 O(n²) 与越界风险） */
        for (int a = 0, b = idx - 1; a < b; a++, b--) {
            LogEntry t = out->items[a];
            out->items[a] = out->items[b];
            out->items[b] = t;
        }
        out->count = (size_t)idx;
        free(wide);
        return idx;
    }
}

void KlogFree(LogList *l)
{
    if (!l)
        return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
}
