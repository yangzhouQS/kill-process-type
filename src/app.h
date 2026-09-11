/* app.h - 全局应用状态与共享定义（唯一全局实例 g_app）
 * 各模块（gui/views/actions）共享的窗口句柄、模式与数据缓存集中于此，
 * 避免跨文件 static 副本与取值函数蔓延。
 */
#ifndef KPT_APP_H
#define KPT_APP_H

#include "common.h"
#include "process.h"

/* 列表视图模式（与页签索引一一对应） */
typedef enum {
    MODE_ALL = 0,   /* 全部进程 */
    MODE_PROC = 1,  /* Node / Python */
    MODE_PORT = 2   /* 端口占用 */
} ListMode;

/* 端口监听条目与进程信息的 Join 结果（端口视图的一行） */
typedef struct {
    DWORD pid;
    DWORD port;
    BOOL tcp;
    BOOL ipv6;
    ProcInfo proc;
    BOOL found;     /* 进程快照中是否找到对应 PID */
    BOOL reserved;  /* TRUE = winnat/系统保留区间行（无进程，杀进程无效） */
    WORD portEnd;   /* reserved 行的区间终点（含） */
} PortRow;

/* 全局应用上下文（gui.c 中定义） */
typedef struct {
    HINSTANCE hInst;
    int dpi;

    /* 控件句柄（WM_CREATE 后有效） */
    HWND hMain, hList, hStatus, hChkAuto;
    HWND hTab, hEditFilter;
    HFONT hFont;

    /* 视图状态与数据缓存 */
    ListMode mode;
    ProcList procs;   /* 全部进程 / Node-Python 视图共用缓存 */
    PortRow *rows;    /* 端口视图缓存 */
    size_t rowCount;
    SYSTEMTIME lastScan;
    int sortCol;      /* 当前排序列（列头索引），-1 = 未排序（快照顺序） */
    BOOL sortDesc;    /* TRUE = 降序 */

    /* 杂项标志 */
    BOOL hideTipShown; /* 首次隐藏到托盘的气泡只提示一次 */
    BOOL wasHidden;    /* 曾隐藏到托盘，恢复显示时需立即刷新 */
} App;

extern App g_app;

/* 主窗口类名/标题（单实例查找与气泡标题共用） */
#define MAIN_WINDOW_CLASS L"KillProcessTypeMainWindow"
#define MAIN_WINDOW_TITLE L"Node/Python 进程终结者"

/* DPI 缩放（以 96 为基准） */
static inline int AppScale(int v) { return MulDiv(v, g_app.dpi, 96); }

#endif
