/* test_ai_spawn.c - 复刻 ai.c 的 kilo 调用条件，打印退出码与完整 stderr
 * 编译: gcc -municode -O2 -Wall -o build\test_ai_spawn.exe tests\test_ai_spawn.c
 */
#include "../src/common.h"
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>

#define KILO_EXE \
    L"H:\\2026code\\2028-amis\\zcode-demo\\kilo-windows-x64-v7.6.2\\kilo.exe"

int wmain(void)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, errFile = INVALID_HANDLE_VALUE;
    WCHAR cmd[8192];
    WCHAR errPath[MAX_PATH + 32];
    WCHAR cwd[MAX_PATH];
    char raw[64 * 1024];
    DWORD rawLen = 0, got;
    ULONGLONG t0;
    BOOL exited = FALSE;

    StringCchPrintfW(cmd, 8192,
        L"\"%ls\" run \"%ls\" --title process-risk-analysis",
        KILO_EXE,
        L"你是Windows进程管理专家。只回复两个字：就绪。进程信息：名称=acrotray.exe；路径=C:\\Program Files\\Adobe\\Acrobat DC\\Acrobat\\acrotray.exe。");

    GetTempPathW(MAX_PATH, errPath);
    StringCchCatW(errPath, MAX_PATH + 32, L"kpt-test-stderr.txt");
    DeleteFileW(errPath);

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        wprintf(L"CreatePipe failed\n");
        return 1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    errFile = CreateFileW(errPath, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = errFile != INVALID_HANDLE_VALUE ? errFile : NULL;
    si.hStdInput = NULL; /* 与 ai.c 一致 */

    GetEnvironmentVariableW(L"USERPROFILE", cwd, MAX_PATH);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, cwd, &si, &pi)) {
        wprintf(L"CreateProcess failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    wprintf(L"spawned pid=%lu\n", (unsigned long)pi.dwProcessId);
    CloseHandle(wr);
    if (errFile != INVALID_HANDLE_VALUE)
        CloseHandle(errFile);

    t0 = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL))
            break;
        if (avail == 0) {
            if (exited)
                break;
            if (GetTickCount64() - t0 > 300000) {
                TerminateProcess(pi.hProcess, 1);
                wprintf(L"TIMEOUT\n");
                break;
            }
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0)
                exited = TRUE;
            continue;
        }
        if (avail > sizeof(raw) - 1 - rawLen)
            avail = sizeof(raw) - 1 - rawLen;
        if (!ReadFile(rd, raw + rawLen, avail, &got, NULL) || !got)
            break;
        rawLen += got;
    }
    raw[rawLen] = '\0';
    {
        DWORD exitCode = 0;
        if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT)
            TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        wprintf(L"kilo exit code: %lu\n", (unsigned long)exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);

    wprintf(L"stdout (%lu bytes):\n%hs\n", (unsigned long)rawLen, raw);

    {
        HANDLE f = CreateFileW(errPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char eb[4096];
            DWORD en = 0;
            if (ReadFile(f, eb, sizeof(eb) - 1, &en, NULL) && en > 0) {
                eb[en] = '\0';
                wprintf(L"stderr (%lu bytes):\n%hs\n", (unsigned long)en, eb);
            } else {
                wprintf(L"stderr: (empty)\n");
            }
            CloseHandle(f);
        } else {
            wprintf(L"stderr file unreadable\n");
        }
    }
    DeleteFileW(errPath);
    return 0;
}
