/* startup.h - 开机自启动（HKCU Run 注册表）接口 */
#ifndef KPT_STARTUP_H
#define KPT_STARTUP_H

#include "common.h"

/* 查询当前是否已开启开机自启动 */
BOOL StartupIsEnabled(void);

/* 写入 HKCU\...\Run："exe完整路径" /tray */
BOOL StartupEnable(void);

/* 删除 Run 值 */
BOOL StartupDisable(void);

#endif
