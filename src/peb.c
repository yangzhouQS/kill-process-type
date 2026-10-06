/* peb.c - PEB 读取实现：
 * NtQueryInformationProcess(ProcessBasicInformation) → PEB → ProcessParameters
 * → CommandLine(UNICODE_STRING) / CurrentDirectory.DosPath。
 * 偏移为业界通用值（x64：PEB+0x20 参数块；cmdline@+0x70、cwd@+0x38）。
 */
#include "common.h"
#include <strsafe.h>

#include "peb.h"

typedef LONG NTSTATUS;

/* 与 ntdll 布局一致的最小定义（x64 对齐） */
typedef struct _MY_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} MY_UNICODE_STRING;

typedef struct _MY_PROCESS_BASIC_INFORMATION {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
} MY_PROCESS_BASIC_INFORMATION;

typedef NTSTATUS (WINAPI *PFN_NtQueryInformationProcess)(
    HANDLE, ULONG, PVOID, ULONG, PULONG *);

#define PEB_PARAMS_OFFSET_X64 0x20
#define PP_CMDLINE_OFFSET_X64 0x70
#define PP_CURDIR_OFFSET_X64 0x38

static BOOL ReadUsString(HANDLE h, const void *paramsBase, size_t offset,
                         WCHAR *out, size_t cchOut)
{
    MY_UNICODE_STRING us;
    SIZE_T n = 0;

    if (!out || !cchOut)
        return FALSE;
    out[0] = L'\0';
    if (!ReadProcessMemory(h, (const BYTE *)paramsBase + offset, &us, sizeof(us), &n) ||
        n < sizeof(us))
        return FALSE;
    if (!us.Buffer || us.Length == 0)
        return FALSE;
    {
        size_t chars = us.Length / sizeof(WCHAR);
        if (chars > cchOut - 1)
            chars = cchOut - 1;
        if (!ReadProcessMemory(h, us.Buffer, out, chars * sizeof(WCHAR), &n) ||
            n < chars * sizeof(WCHAR)) {
            out[0] = L'\0';
            return FALSE;
        }
        out[chars] = L'\0';
    }
    return TRUE;
}

BOOL PebQuery(DWORD pid, WCHAR *cmd, size_t cchCmd, WCHAR *cwd, size_t cchCwd)
{
    HANDLE h;
    MY_PROCESS_BASIC_INFORMATION pbi;
    NTSTATUS st;
    PVOID params = NULL;
    SIZE_T n = 0;
    BOOL ok = FALSE;
    HMODULE ntdll;
    PFN_NtQueryInformationProcess pNtQIP;

    if (cmd && cchCmd)
        cmd[0] = L'\0';
    if (cwd && cchCwd)
        cwd[0] = L'\0';

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return FALSE;
    pNtQIP = (PFN_NtQueryInformationProcess)(void *)(INT_PTR)
        GetProcAddress(ntdll, "NtQueryInformationProcess");
    if (!pNtQIP)
        return FALSE;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h)
        return FALSE;

    ZeroMemory(&pbi, sizeof(pbi));
    st = pNtQIP(h, 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), NULL);
    if (st >= 0 && pbi.PebBaseAddress) {
        if (ReadProcessMemory(h, (const BYTE *)pbi.PebBaseAddress + PEB_PARAMS_OFFSET_X64,
                              &params, sizeof(params), &n) && n == sizeof(params) && params) {
            BOOL okCmd = TRUE, okCwd = TRUE;
            if (cmd && cchCmd)
                okCmd = ReadUsString(h, params, PP_CMDLINE_OFFSET_X64, cmd, cchCmd);
            if (cwd && cchCwd)
                okCwd = ReadUsString(h, params, PP_CURDIR_OFFSET_X64, cwd, cchCwd);
            ok = okCmd || okCwd;
        }
    }
    CloseHandle(h);
    return ok;
}
