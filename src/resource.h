/* resource.h - 资源与命令 ID 定义
 * 注意：IDI_APP 必须与 res/app.rc 中的 "101 ICON" 数值保持一致
 * （app.rc 有意不 include 本文件，避免 windres 预处理路径问题）
 */
#ifndef KPT_RESOURCE_H
#define KPT_RESOURCE_H

/* 图标资源 ID */
#define IDI_APP 101

/* 托盘菜单命令（菜单在运行时用 AppendMenuW 创建） */
#define IDM_TRAY_REFRESH   1101
#define IDM_TRAY_KILL_NODE 1102
#define IDM_TRAY_KILL_PY   1103
#define IDM_TRAY_SHOW      1104
#define IDM_TRAY_EXIT      1105
#define IDM_TRAY_AUTOSTART 1106
#define IDM_TRAY_SETTINGS  1107
#define IDM_TRAY_ORPHAN    1108

/* 主窗口控件 ID */
#define IDC_BTN_REFRESH   2001
#define IDC_BTN_KILL_SEL  2002
#define IDC_BTN_KILL_NODE 2003
#define IDC_BTN_KILL_PY   2004
#define IDC_CHK_AUTO      2005
#define IDC_LIST          2006
#define IDC_EDIT_FILTER   2007
#define IDC_TAB           2008
#define IDC_CHK_TREE      2010
#define IDC_BTN_AI_LOG    2011
#define IDC_BTN_AI_BATCH  2012
#define IDC_BTN_AI_DIAG   2013
#define IDC_CHK_PROJECT   2014

/* 列表行右键菜单命令 */
#define IDM_LIST_COPY_PATH  2101
#define IDM_LIST_COPY_FIX   2102
#define IDM_LIST_ELEVATE_FIX 2103
#define IDM_LIST_AI_ANALYZE 2104
#define IDM_LIST_OPEN_URL   2105
#define IDM_LIST_COPY_URL   2106
#define IDM_LIST_SHOW_IN_EXPLORER 2107
#define IDM_LIST_COPY_CMD   2108
#define IDM_LIST_OPEN_IN_TERMINAL 2109

/* AI 评估对话框控件 */
#define IDAI_KILL  2201
#define IDAI_CLOSE 2202

/* 设置窗口控件 */
#define IDC_SET_AUTOSTART   2301
#define IDC_SET_STARTMIN    2302
#define IDC_SET_THEME_AUTO  2303
#define IDC_SET_THEME_LIGHT 2304
#define IDC_SET_THEME_DARK  2305
#define IDC_SET_AUTOEN      2306
#define IDC_SET_AUTOIV      2307
#define IDC_SET_BALLOON     2308
#define IDC_SET_CLOSE       2309
#define IDC_SET_AI_RECOMMEND 2313

/* 设置窗口：孤儿进程清理 */
#define IDC_SET_ORPHANEN  2310
#define IDC_SET_ORPHANIV  2311
#define IDC_SET_ORPHANNP  2312

/* 自定义消息与定时器（需 windows.h，务必在 common.h 之后包含） */
#define WM_APP_TRAY (WM_APP + 1)
#define TIMER_AUTO_REFRESH 1
#define TIMER_ORPHAN 2     /* 孤儿进程定时清理（分钟级） */

#endif
