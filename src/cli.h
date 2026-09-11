/* cli.h - 无头 CLI 模式（供 AI 工具链集成）：/list /ports /kill <pid> */
#ifndef KPT_CLI_H
#define KPT_CLI_H

#include "common.h"

/* 命中 CLI 参数则执行并返回 TRUE（wWinMain 随即退出，不进 GUI） */
BOOL CliDispatch(const WCHAR *cmdLine);

#endif
