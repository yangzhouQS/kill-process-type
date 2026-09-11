/* theme.h - 深色主题（移植自 memreduct 的主题理念：跟随系统 + 手动切换） */
#ifndef KPT_THEME_H
#define KPT_THEME_H

#include "common.h"
#include <commctrl.h>

typedef enum {
    THEME_AUTO = 0,  /* 跟随系统（AppsUseLightTheme） */
    THEME_LIGHT = 1,
    THEME_DARK = 2
} ThemeMode;

/* ConfigInit 之后调用一次 */
void ThemeInit(void);

ThemeMode ThemeGetMode(void);
/* 切换模式：写配置并重算（调用方负责对已存在窗口重新 Apply） */
void ThemeSetMode(ThemeMode mode);

/* 当前生效是否深色 */
BOOL ThemeIsDark(void);

/* ---------- 各类控件应用（dark 生效；light 复位为系统默认） ---------- */
void ThemeApplyFrame(HWND hwnd);               /* DWM 沉浸式标题栏 */
void ThemeApplyListView(HWND lv);              /* 列表背景/文字 */
void ThemeApplyStatusBar(HWND sb);
void ThemeApplyTabControl(HWND tab);           /* 安装子类化 + OWNERDRAW */
void ThemeApplyEdit(HWND edit);
/* 按钮组 ownerdraw 开关（dark=自绘深色，light=还原系统） */
void ThemeApplyButtons(HWND parent, const INT *ids, int count);

/* ---------- 父窗口消息路由（各 WndProc 转发，非 dark 时安全直通） ---------- */
/* WM_ERASEBKGND：dark 填充返回 TRUE；否则 FALSE（走默认） */
BOOL ThemeOnEraseBkgnd(HWND hwnd, HDC hdc);
/* WM_CTLCOLORSTATIC/EDIT/BTN：返回画刷；非 dark 返回 NULL（走默认） */
HBRUSH ThemeOnCtlColor(HWND ctrl, HDC hdc);
/* WM_DRAWITEM（ownerdraw 按钮/Tab 项）：TRUE 已处理 */
BOOL ThemeOnDrawItem(LPARAM lp);
/* ListView 表头的 WM_NOTIFY(NM_CUSTOMDRAW)：TRUE 已处理并写入 *result */
BOOL ThemeOnHeaderNotify(LPNMHDR hdr, LRESULT *result);
/* WM_SETTINGCHANGE：lParam == ImmersiveColorSet 时 auto 模式重算（返回 TRUE 表示已切换） */
BOOL ThemeOnSettingChange(const WCHAR *what);

/* 供自绘使用（settings 窗口等） */
COLORREF ThemeGetBackColor(void);
COLORREF ThemeGetTextColor(void);
HBRUSH ThemeGetBackBrush(void);

#endif
