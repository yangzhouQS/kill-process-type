/* process.h - 进程枚举与终止接口 */
#ifndef KPT_PROCESS_H
#define KPT_PROCESS_H

#include "common.h"

/* AI 风险等级（WP3 批量扫描填充，在 process.h 因为 ProcInfo 引用） */
typedef enum {
    RISK_UNKNOWN = 0,
    RISK_LOW,
    RISK_MED,
    RISK_HIGH
} RiskLevel;

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
    WCHAR cmdline[208];    /* 完整命令行（仅 node/python 采集，peb.c；其余为空串） */
    RiskLevel aiRisk;      /* AI 风险等级（WP3 批量扫描填充，默认 UNKNOWN） */
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

/* 扫描全部进程；返回数量，快照失败返回 -1 */
int ScanAllProcesses(ProcList *out);

/* 扫描孤儿进程（父进程已退出、仍存活的残留进程）；返回数量。
 * nodePythonOnly=TRUE 时仅 node/python；
 * 任何模式均自动排除：pid 0/4、ppid==0、本进程自身，
 * 以及路径位于系统 Windows 目录下的进程（全量模式的安全护栏）。 */
int ScanOrphanProcesses(ProcList *out, BOOL nodePythonOnly);

/* 枚举全部进程（不做类型过滤，供端口模式 Join 进程信息用） */
int ScanAllProcesses(ProcList *out);

/* 释放列表内存 */
void FreeProcList(ProcList *l);

/* 按进程名分类：node.exe / python*.exe / pythonw*.exe */
ProcType ClassifyName(const WCHAR *exeName);

/* 逐个 TerminateProcess；结果写入 res。
 * errOut 可为 NULL；非 NULL 时 errOut[i] = 第 i 个 PID 的结果
 * （0 = 成功，否则 GetLastError 错误码），供日志记录使用。 */
void KillPids(const DWORD *pids, size_t count, KillResult *res, DWORD *errOut);

/* 类型显示名 */
const WCHAR *ProcTypeLabel(ProcType t);

#endif
