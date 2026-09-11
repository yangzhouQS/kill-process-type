/* tray.h - 托盘图标接口 */
#ifndef KPT_TRAY_H
#define KPT_TRAY_H

#include "common.h"

/* 初始化托盘数据（不显示） */
void TrayInit(HWND owner, HINSTANCE hInst, UINT callbackMsg);

/* 添加/删除托盘图标 */
BOOL TrayAdd(void);
void TrayRemove(void);

/* 气泡通知（失败静默忽略） */
void TrayShowBalloon(const WCHAR *title, const WCHAR *text);

/* 显示/隐藏主窗口 */
void TrayToggleWindow(HWND owner);

/* 处理 WM_APP_TRAY 消息，返回 0 表示已处理 */
LRESULT TrayHandleMessage(HWND owner, WPARAM wp, LPARAM lp);

#endif
