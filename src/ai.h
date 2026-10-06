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

/* ---------- WP1: AI 核心层扩展（v5.2） ---------- */

/* 批量任务进度通知：wParam=已完成批次, lParam=总批次（调用线程投递） */
#define WM_APP_AI_PROGRESS (WM_APP + 3)

/* Prompt 模板 ID（多套场景复用同一执行链路） */
typedef enum {
    AIPROMPT_RISK_SINGLE = 0,  /* 单进程风险评估（现有） */
    AIPROMPT_RISK_BATCH,       /* 批量风险分级（JSON 输出） */
    AIPROMPT_LOG_REVIEW,       /* 日志复盘 */
    AIPROMPT_DIAG_GLOBAL,      /* 全局诊断（四区块+动作清单） */
    AIPROMPT_AI_QUERY,         /* CLI 自然语言查询 */
    AIPROMPT_PORT_SUGGEST,     /* CLI 端口故障分析 */
    AIPROMPT_COUNT
} AiPromptId;

/* 取指定场景的 Prompt 模板（静态常量，调用方不可修改/释放） */
const WCHAR *AiGetPrompt(AiPromptId id);

/* 从模型自由文本中提取首个完整 JSON 对象/数组（括号配对，字符串感知）。
 * 支持围栏 ```json ... ``` 与裸输出；失败返回 NULL。
 * 返回 malloc 的宽字符串，调用方 free。 */
WCHAR *AiExtractJson(const WCHAR *answer);

/* 上下文限流：超 maxChars 时在最后一个完整行处截断并追加省略标注。
 * 原地修改 buf（保证 NUL 终止），返回实际长度。 */
size_t AiTruncateContext(WCHAR *buf, size_t maxChars);

/* 组装进程风险分析提示词（自动附带该 PID 的监听端口） */
void AiBuildPrompt(const ProcInfo *p, WCHAR *buf, size_t cch);

/* 异步执行 kilo run "<prompt>"（--format json --thinking），完成后向
 * hwndNotify 投递 WM_APP_AI_DONE（lParam = AiResult*）。
 * 同时仅允许一个分析任务。返回 TRUE 表示已启动。 */
BOOL AiStartAnalysis(HWND hwndNotify, const WCHAR *prompt);

BOOL AiBusy(void);

#endif
