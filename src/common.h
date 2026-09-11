/* common.h - 全局编译环境统一定义（所有 .c 首先包含本文件） */
#ifndef KPT_COMMON_H
#define KPT_COMMON_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7+ */
#endif
#define PSAPI_VERSION 1 /* 使用经典 GetProcessMemoryInfo，链接 psapi.lib */

#include <windows.h>

#endif
