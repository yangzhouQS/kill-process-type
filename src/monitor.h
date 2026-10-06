/* monitor.h - 进程时序采集（WP6：CPU/内存环形缓冲，P2 异常监控的地基） */
#ifndef KPT_MONITOR_H
#define KPT_MONITOR_H

#include "common.h"

#define MONITOR_SLOTS 60   /* 60 格 × 2s = 2 分钟窗口 */
#define MONITOR_MAX_PIDS 64

typedef struct {
    DWORD pid;
    ULONGLONG memHist[MONITOR_SLOTS]; /* 工作集字节 */
    double cpuHist[MONITOR_SLOTS];    /* CPU% */
    int head;                          /* 环形写入位置 */
    int filled;                        /* 已填充格数（≤MONITOR_SLOTS） */
} MonitorSlot;

/* 启动/停止采样线程（幂等） */
void MonitorStart(void);
void MonitorStop(void);

/* 添加/移除监控 PID（超上限 LRU 淘汰） */
void MonitorAdd(DWORD pid);
void MonitorRemove(DWORD pid);

/* 取某 PID 的时序副本（返回已填充格数，0=不在监控中） */
int MonitorGetSeries(DWORD pid, ULONGLONG *memOut, double *cpuOut, int maxSlots);

/* 当前监控的 PID 数 */
int MonitorCount(void);

#endif
