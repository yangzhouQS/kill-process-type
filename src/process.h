/* process.h - 进程枚举与终止接口 */
#ifndef KPT_PROCESS_H
#define KPT_PROCESS_H

#include "common.h"

typedef enum {
    PT_NONE = 0,
    PT_NODE = 1,
    PT_PYTHON = 2
} ProcType;

typedef struct {
    DWORD pid;
    DWORD ppid;
    WCHAR name[64];        /* 主进程名，如 node.exe */
    WCHAR path[MAX_PATH];  /* 完整可执行路径，获取失败为空串 */
    unsigned long long memBytes; /* 工作集大小，失败为 0 */
    ProcType type;
} ProcInfo;

typedef struct {
    ProcInfo *items;
    size_t count;
    size_t cap;
} ProcList;

typedef struct {
    int okCount;
    int failCount;
    BOOL truncated;         /* 失败明细因缓冲区写满而省略了后续行 */
    WCHAR failDetail[1024]; /* 失败明细，每行一条 */
} KillResult;

/* 扫描全部 node/python 进程；返回匹配数量，快照失败返回 -1 */
int ScanProcesses(ProcList *out);

/* 枚举全部进程（不做类型过滤，供端口模式 Join 进程信息用） */
int ScanAllProcesses(ProcList *out);

/* 释放列表内存 */
void FreeProcList(ProcList *l);

/* 按进程名分类：node.exe / python*.exe / pythonw*.exe */
ProcType ClassifyName(const WCHAR *exeName);

/* 逐个 TerminateProcess；结果写入 res */
void KillPids(const DWORD *pids, size_t count, KillResult *res);

/* 类型显示名 */
const WCHAR *ProcTypeLabel(ProcType t);

#endif
