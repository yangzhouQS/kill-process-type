/* actions.h - 用户动作：终止进程、复制路径、列表右键菜单 */
#ifndef KPT_ACTIONS_H
#define KPT_ACTIONS_H

#include "app.h"

/* 终止指定类型的全部进程（带确认弹窗） */
void ActionsKillAllOfType(ProcType t);

/* 终止列表中勾选的进程（端口视图按 PID 去重） */
void ActionsKillSelected(void);

/* 复制全部选中行的可执行路径到剪贴板（多行以 CRLF 连接） */
void ActionsCopySelectedPaths(HWND owner);

/* 复制 winnat 保留端口的修复命令块（配合右键保留区间行使用） */
void ActionsCopyFix(HWND owner);

/* 一键提权修复保留端口（UAC 确认后自动执行 winnat 重启 + 端口固定） */
void ActionsElevatedFix(HWND owner);

/* AI 风险评估：右键菜单入口（rowIndex 来自右键命中行） */
void ActionsAiAnalyze(int rowIndex);
void ActionsAiMenuCommand(HWND hwnd);

/* AI 分析完成回调（WM_APP_AI_DONE 路由；lp 为堆串，本函数负责 free） */
void ActionsAiDone(WPARAM wp, LPARAM lp);

/* 主题变更广播：AI 评估窗口打开时重新应用 */
void ActionsAiOnThemeChanged(void);

/* 列表行右键菜单（WM_CONTEXTMENU 路由入口） */
void ActionsOnListContextMenu(HWND hwnd, LPARAM lp);

#endif
