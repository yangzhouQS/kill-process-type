/* clipread.c - 独立进程读取剪贴板 CF_UNICODETEXT 并以 UTF-8 打印
 * 用途：UI 自动化测试中验证复制结果（避免 PowerShell OLE 剪贴板环境问题）
 * 编译: gcc -O2 -o build\clipread.exe tests\clipread.c
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(NULL))
            break;
        if (i == 9) {
            printf("(open clipboard failed: %lu)\n", (unsigned long)GetLastError());
            return 1;
        }
        Sleep(150);
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        printf("(no unicode text)\n");
        CloseClipboard();
        return 0;
    }
    const wchar_t *p = (const wchar_t *)GlobalLock(h);
    if (p) {
        int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, NULL, 0, NULL, NULL);
        char *buf = (char *)malloc(n > 0 ? (size_t)n + 1 : 1);
        if (buf) {
            WideCharToMultiByte(CP_UTF8, 0, p, -1, buf, n, NULL, NULL);
            buf[n > 0 ? n : 0] = '\0';
            printf("%s", buf);
            free(buf);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return 0;
}
