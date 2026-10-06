/* klog.h - 终止进程日志：追加式落盘 + 供视图加载 */
#ifndef KPT_KLOG_H
#define KPT_KLOG_H

#include "common.h"
#include "app.h" /* LogEntry/LogList */

#define KLOG_MAX_ENTRIES 500 /* 视图最多加载的最近记录条数 */
#define KLOG_MAX_BYTES (512 * 1024) /* 超过则滚动为 .old */

/* ConfigInit 之后调用：解析日志路径（与配置文件同目录）+ 超限滚动 */
void KlogInit(void);

/* 追加一条记录（UTF-8，制表符分隔；info 可为 NULL，名称记为"(未知)"） */
void KlogWrite(const WCHAR *source, const ProcInfo *info, DWORD pid,
               BOOL ok, DWORD err);

/* 加载最近 KLOG_MAX_ENTRIES 条到 out（新记录在前）；返回条数，失败返回 -1 */
int KlogLoad(LogList *out);

/* 释放列表 */
void KlogFree(LogList *l);

/* 当前日志文件路径 */
const WCHAR *KlogGetPath(void);

#endif
