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

/* 孤儿进程清理：autoMode=TRUE 定时静默清理（气泡汇总），
 * FALSE 手动触发（无孤儿提示/有孤儿列清单确认） */
void ActionsCleanOrphans(BOOL autoMode);

/* AI 风险评估：右键菜单入口（rowIndex 来自右键命中行） */
void ActionsAiAnalyze(int rowIndex);
void ActionsAiMenuCommand(HWND hwnd);

/* AI 分析完成回调（WM_APP_AI_DONE 路由；lp 为堆串，本函数负责 free） */
void ActionsAiDone(WPARAM wp, LPARAM lp);

/* 主题变更广播：AI 评估窗口打开时重新应用 */
void ActionsAiOnThemeChanged(void);

/* 列表行右键菜单（WM_CONTEXTMENU 路由入口） */
void ActionsOnListContextMenu(HWND hwnd, LPARAM lp);

/* 日志页签 AI 复盘：收集勾选日志行→PROMPT_LOG_REVIEW→报告窗口 */
void ActionsAiLogReview(HWND hwnd);

/* WP3: 批量 AI 风险扫描（勾选行→分批调 kilo→回填 aiRisk→重建列表） */
void ActionsAiBatchScan(HWND hwnd);

/* WP5: AI 全局诊断窗口 */
void ActionsAiDiagOpen(HWND hwnd);

/* WP10: AI 推荐配置（采集统计→kilo→推荐清单） */
void ActionsAiConfigRecommend(HWND hwnd);

/* WP5: 诊断结果路由（WM_APP_AI_DONE 时检查 pending 并派发） */
void ActionsDiagCheckPending(WPARAM wp, LPARAM lp);

/* WP3: 解析批量扫描结果并回填（WM_APP_AI_DONE 后调用） */
void ActionsAiBatchApply(WPARAM wp, LPARAM lp);

/* Dev 快捷操作（右键菜单项，gui.c 路由） */
void OpenRowUrl(HWND owner, BOOL copyOnly);
void ShowRowInExplorer(int rowIdx);
void OpenRowTerminal(int rowIdx);
void CopyRowCmdline(HWND owner, int rowIdx);

/* 取最近一次右键命中的行索引（Dev 快捷操作定位用） */
int ActionsGetContextRow(void);

/* WP13: 智能重启 — 杀进程后用原命令行+cwd 拉起 */
void ActionsSmartRestart(HWND hwnd, int rowIdx);

/* WP9: AI 清理策略 — 孤儿分档（auto/manual/forbid）+ 确认执行 */
void ActionsAiCleanStrategy(HWND hwnd);

/* WP11: 时序异常检测（WM_TIMER 周期调用） */
void ActionsAnomalyCheck(void);

/* WP12: 基线保存/对比 */
void ActionsSaveBaseline(HWND hwnd);
void ActionsCompareBaseline(HWND hwnd);

/* WP7: AI 对话面板（显示/隐藏切换） */
void ChatPanelToggle(HWND mainHwnd);

#endif
