/* ai.h - AI 风险评估：无头调用 kilo run（本地 CLI 代理） */
#ifndef KPT_AI_H
#define KPT_AI_H

#include "common.h"
#include "process.h"

/* 完成通知：wParam=1 成功 / 0 失败；lParam = 堆分配的 AiResult，
 * 接收方负责 free 各字段与结构本身 */
#define WM_APP_AI_DONE (WM_APP + 2)

/* kilo run --format json 事件流的解析结果 */
typedef struct {
    WCHAR *answer;   /* 最终回答（text 事件拼接），失败为 NULL */
    WCHAR *thinking; /* 思考过程（reasoning 事件拼接），可为 NULL */
    WCHAR *diag;     /* 失败诊断（退出码 + stderr），成功为 NULL */
} AiResult;

/* 释放结果结构（各字段与本体） */
void AiResultFree(AiResult *r);

/* 组装进程风险分析提示词（自动附带该 PID 的监听端口） */
void AiBuildPrompt(const ProcInfo *p, WCHAR *buf, size_t cch);

/* 异步执行 kilo run "<prompt>"（--format json --thinking），完成后向
 * hwndNotify 投递 WM_APP_AI_DONE（lParam = AiResult*）。
 * 同时仅允许一个分析任务。返回 TRUE 表示已启动。 */
BOOL AiStartAnalysis(HWND hwndNotify, const WCHAR *prompt);

BOOL AiBusy(void);

#endif
