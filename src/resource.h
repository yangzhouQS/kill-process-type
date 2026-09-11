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

/* 主窗口控件 ID */
#define IDC_BTN_REFRESH   2001
#define IDC_BTN_KILL_SEL  2002
#define IDC_BTN_KILL_NODE 2003
#define IDC_BTN_KILL_PY   2004
#define IDC_CHK_AUTO      2005
#define IDC_LIST          2006
#define IDC_EDIT_FILTER   2007
#define IDC_TAB           2008

/* 列表行右键菜单命令 */
#define IDM_LIST_COPY_PATH  2101
#define IDM_LIST_COPY_FIX   2102
#define IDM_LIST_ELEVATE_FIX 2103
#define IDM_LIST_AI_ANALYZE 2104

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

/* 自定义消息与定时器（需 windows.h，务必在 common.h 之后包含） */
#define WM_APP_TRAY (WM_APP + 1)
#define TIMER_AUTO_REFRESH 1

#endif
