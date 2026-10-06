/* monitor.c - 进程时序采集实现（WP6）
 * 独立采样线程每 2s 快照关注集：GetProcessTimes 差分 CPU%、
 * GetProcessMemoryInfo 工作集。环形缓冲存储。
 */
#include "common.h"
#include <psapi.h>
#include <strsafe.h>
#include <stdlib.h>

#include "monitor.h"

static MonitorSlot g_slots[MONITOR_MAX_PIDS];
static int g_slotCount = 0;
static volatile BOOL g_running = FALSE;
static HANDLE g_thread = NULL;
static CRITICAL_SECTION g_cs;

/* 取某 PID 的 FILETIME 快照（用于 CPU 差分） */
static BOOL GetCpuTime(DWORD pid, ULARGE_INTEGER *out)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    FILETIME ftCreate, ftExit, ftKernel, ftUser;

    if (!h)
        return FALSE;
    BOOL ok = GetProcessTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser);
    CloseHandle(h);
    if (!ok)
        return FALSE;
    out->QuadPart = (ULONGLONG)ftKernel.dwHighDateTime << 32 | ftKernel.dwLowDateTime;
    out->QuadPart += (ULONGLONG)ftUser.dwHighDateTime << 32 | ftUser.dwLowDateTime;
    return TRUE;
}

static ULONGLONG GetMemBytes(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    PROCESS_MEMORY_COUNTERS pmc;

    if (!h)
        return 0;
    ZeroMemory(&pmc, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    ULONGLONG r = 0;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
        r = (ULONGLONG)pmc.WorkingSetSize;
    CloseHandle(h);
    return r;
}

static DWORD WINAPI MonitorThread(LPVOID param)
{
    ULARGE_INTEGER prevCpu[MONITOR_MAX_PIDS];
    ULONGLONG prevTick = 0;

    (void)param;
    while (g_running) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsed = prevTick ? (now - prevTick) : 0;

        EnterCriticalSection(&g_cs);
        for (int i = 0; i < g_slotCount; i++) {
            MonitorSlot *s = &g_slots[i];
            ULARGE_INTEGER cpu;
            ULONGLONG mem = GetMemBytes(s->pid);

            if (mem == 0 && !GetCpuTime(s->pid, &cpu)) {
                /* 进程可能已退出：标记为 0 */
                s->memHist[s->head] = 0;
                s->cpuHist[s->head] = 0;
            } else {
                s->memHist[s->head] = mem;
                if (GetCpuTime(s->pid, &cpu) && elapsed > 0 && prevCpu[i].QuadPart > 0) {
                    double cpuMs = (double)(cpu.QuadPart - prevCpu[i].QuadPart) / 10000.0;
                    s->cpuHist[s->head] = cpuMs * 1000.0 / (double)elapsed;
                    if (s->cpuHist[s->head] > 400)
                        s->cpuHist[s->head] = 400; /* 多核上限 */
                } else {
                    s->cpuHist[s->head] = 0;
                }
                prevCpu[i] = cpu;
            }
            s->head = (s->head + 1) % MONITOR_SLOTS;
            if (s->filled < MONITOR_SLOTS)
                s->filled++;
        }
        LeaveCriticalSection(&g_cs);
        prevTick = now;

        Sleep(2000);
    }
    return 0;
}

void MonitorStart(void)
{
    if (g_running)
        return;
    InitializeCriticalSection(&g_cs);
    g_running = TRUE;
    g_thread = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
}

void MonitorStop(void)
{
    if (!g_running)
        return;
    g_running = FALSE;
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    DeleteCriticalSection(&g_cs);
}

void MonitorAdd(DWORD pid)
{
    if (!g_running || pid == 0)
        return;
    EnterCriticalSection(&g_cs);
    /* 已在监控中则跳过 */
    for (int i = 0; i < g_slotCount; i++)
        if (g_slots[i].pid == pid) {
            LeaveCriticalSection(&g_cs);
            return;
        }
    /* 满则淘汰第一个（简单 LRU：实际是 FIFO，够用） */
    if (g_slotCount >= MONITOR_MAX_PIDS) {
        g_slots[0].pid = pid;
        g_slots[0].head = 0;
        g_slots[0].filled = 0;
        memmove(&g_slots[0], &g_slots[1],
                (MONITOR_MAX_PIDS - 1) * sizeof(MonitorSlot));
        g_slotCount--;
    }
    ZeroMemory(&g_slots[g_slotCount], sizeof(MonitorSlot));
    g_slots[g_slotCount].pid = pid;
    g_slotCount++;
    LeaveCriticalSection(&g_cs);
}

void MonitorRemove(DWORD pid)
{
    if (!g_running)
        return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_slotCount; i++) {
        if (g_slots[i].pid == pid) {
            memmove(&g_slots[i], &g_slots[i + 1],
                    (g_slotCount - i - 1) * sizeof(MonitorSlot));
            g_slotCount--;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
}

int MonitorGetSeries(DWORD pid, ULONGLONG *memOut, double *cpuOut, int maxSlots)
{
    int n = 0;

    if (!g_running || !memOut || !cpuOut || maxSlots <= 0)
        return 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_slotCount; i++) {
        if (g_slots[i].pid == pid) {
            MonitorSlot *s = &g_slots[i];
            n = s->filled;
            if (n > maxSlots)
                n = maxSlots;
            /* 从最旧到最新复制 */
            for (int k = 0; k < n; k++) {
                int src = (s->head - s->filled + k + MONITOR_SLOTS * 2) % MONITOR_SLOTS;
                memOut[k] = s->memHist[src];
                cpuOut[k] = s->cpuHist[src];
            }
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

int MonitorCount(void)
{
    return g_slotCount;
}
