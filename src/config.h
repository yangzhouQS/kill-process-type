/* config.h - 配置持久化（INI，移植自 memreduct 的配置理念，轻量实现） */
#ifndef KPT_CONFIG_H
#define KPT_CONFIG_H

#include "common.h"

/* 解析配置路径：exe 同目录 kill-process-type.ini（便携），
 * 不可写时回退 %APPDATA%\kill-process-type.ini。wWinMain 最先调用。 */
void ConfigInit(void);

/* 配置键（节固定为 main）：
 *   AutoRefresh / AutoRefreshInterval / StartMinimized
 *   Theme(0=跟随系统 1=浅色 2=深色) / BalloonNotify
 *   WinX / WinY / WinW / WinH（主窗口位置持久化） */

BOOL ConfigGetBool(const WCHAR *key, BOOL def);
LONG ConfigGetLong(const WCHAR *key, LONG def);
void ConfigSetBool(const WCHAR *key, BOOL val);
void ConfigSetLong(const WCHAR *key, LONG val);

/* 当前配置文件完整路径（诊断用） */
const WCHAR *ConfigGetPath(void);

#endif
