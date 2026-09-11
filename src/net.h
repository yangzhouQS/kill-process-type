/* net.h - 端口监听扫描接口（iphlpapi 双栈 TCP/UDP） */
#ifndef KPT_NET_H
#define KPT_NET_H

#include "common.h"

typedef struct {
    DWORD pid;
    DWORD port;   /* 本地端口（主机字节序） */
    BOOL  tcp;    /* TRUE=TCP, FALSE=UDP */
    BOOL  ipv6;   /* 本地地址为 IPv6 */
} NetEntry;

typedef struct {
    NetEntry *items;
    size_t count;
    size_t cap;
} NetList;

/* 扫描全部 TCP LISTEN + UDP 监听，按（端口, 协议）升序排序；
 * 返回条目数，失败返回 -1 */
int ScanListenPorts(NetList *out);

/* 释放列表内存 */
void FreeNetList(NetList *l);

/* ---------------- 系统保留端口区间（winnat / Hyper-V / 管理员保留） ----------------
 * 这类区间会监听 EACCES（权限拒绝），没有对应进程，杀进程无法释放。
 */

typedef struct {
    WORD start;   /* 起始端口（含） */
    WORD end;     /* 结束端口（含） */
    BOOL tcp;
    BOOL ipv6;
} PortRange;

typedef struct {
    PortRange *items;
    size_t count;
    size_t cap;
} PortRangeList;

/* 解析 netsh excludedportrange（v4/v6 × tcp/udp 四次查询）；
 * 返回区间数，失败返回 -1，无数据返回 0 */
int ScanReservedPortRanges(PortRangeList *out);

/* 释放区间列表内存 */
void FreePortRangeList(PortRangeList *l);

#endif
