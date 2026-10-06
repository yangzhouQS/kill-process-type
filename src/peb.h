/* peb.h - 读取目标进程的完整命令行与工作目录（PEB / RTL_USER_PROCESS_PARAMETERS） */
#ifndef KPT_PEB_H
#define KPT_PEB_H

#include "common.h"

/* 查询指定进程的完整命令行与当前工作目录。
 * cmd/cwd 可为 NULL（不需要时跳过）；失败或不可读时输出空串并返回 FALSE。
 * 权限：目标进程需可 OpenProcess(QUERY_LIMITED|VM_READ)（同用户普通进程即可）。 */
BOOL PebQuery(DWORD pid, WCHAR *cmd, size_t cchCmd, WCHAR *cwd, size_t cchCwd);

#endif
