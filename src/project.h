/* project.h - 项目根目录识别（WP8：PEB cwd 向上找 package.json 等） */
#ifndef KPT_PROJECT_H
#define KPT_PROJECT_H

#include "common.h"

/* 从 cwd 向上找项目根（最多向上 3 级）。
 * 项目根 = 含 package.json / pyproject.toml / requirements.txt 的最近目录。
 * 返回项目根路径（WCHAR*，静态缓冲区，不需 free）；找不到返回 NULL。 */
const WCHAR *FindProjectRoot(const WCHAR *cwd);

/* 取某进程的项目归属（PEB 查 cwd → FindProjectRoot）。
 * 返回项目根路径或 NULL；输出缓冲区由调用方提供。 */
BOOL GetProcessProject(DWORD pid, WCHAR *out, size_t cch);

#endif
