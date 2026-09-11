/* richtext.h - Rich Edit 渲染工具：Markdown 子集 → RTF 报告 */
#ifndef KPT_RICHTEXT_H
#define KPT_RICHTEXT_H

#include "common.h"

/* 创建 Rich Edit 控件（自动加载 msftedit/riched20）；
 * 失败返回 NULL，调用方可退回普通 EDIT */
HWND RichTextCreate(HWND parent, int id);

/* 以 RTF 流入方式设置内容（rtf 为 GBK/ASCII 字节串） */
void RichTextSetRtf(HWND hRich, const char *rtf);

/* 组装 AI 评估报告 RTF：
 *   【提示词】灰色等宽
 *   【思考过程】灰色斜体（thinking 为空则省略该节）
 *   【分析结果】Markdown 渲染（diag 非空时以错误样式替代 answer）
 * 返回 malloc 的 RTF 串，调用方负责 free；失败返回 NULL */
char *RichTextBuildAiReport(const WCHAR *prompt, const WCHAR *thinking,
                            const WCHAR *answer, const WCHAR *diag);

#endif
