/* settings.h - 设置窗口（配置/主题/自启/通知，改动实时生效） */
#ifndef KPT_SETTINGS_H
#define KPT_SETTINGS_H

#include "common.h"

/* 显示设置窗口（单实例；不存在则创建） */
void SettingsShow(void);

/* 窗口是否打开 */
BOOL SettingsIsVisible(void);

/* 主题变更广播：窗口打开时重新应用（换肤/系统自动切换联动） */
void SettingsOnThemeChanged(void);

#endif
