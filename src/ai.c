/* ai.c - AI 风险评估实现：CreateProcessW 拉起
 *   kilo run "<prompt>" --format json --thinking
 * 逐行解析 JSON 事件流（reasoning → 思考过程，text → 最终回答），
 * 工作线程轮询读取 + 超时看护 + 3 次退避重试，经 WM_APP_AI_DONE 回投 UI。
 */
#include "common.h"
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "ai.h"
#include "net.h"
#include "app.h"

/* kilo CLI 候选路径：环境变量 KILO_EXE 优先，其次本仓库发行包、
 * 常见安装位置，最后 PATH 中的 kilo */
static const WCHAR *const kKiloCandidates[] = {
    L"H:\\2026code\\2028-amis\\zcode-demo\\kilo-windows-x64-v7.6.2\\kilo.exe",
    L"D:\\kilo\\kilo-windows-x64\\kilo.exe",
    L"kilo",
};

#define AI_TIMEOUT_MS 300000 /* 5 分钟看护上限 */
#define AI_OUT_CAP   (256 * 1024)
#define AI_WCAP      (65536) /* thinking/answer 宽字符上限 */

static BOOL BuildKiloCmd(WCHAR *cmd, size_t cch, const WCHAR *prompt)
{
    WCHAR env[MAX_PATH];
    DWORD n;

    n = GetEnvironmentVariableW(L"KILO_EXE", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        StringCchPrintfW(cmd, cch,
                         L"\"%ls\" run \"%ls\" --format json --thinking --title process-risk-analysis",
                         env, prompt);
        return TRUE;
    }
    for (int i = 0; i < (int)(sizeof(kKiloCandidates) / sizeof(kKiloCandidates[0])); i++) {
        const WCHAR *cand = kKiloCandidates[i];
        if (wcschr(cand, L'\\') == NULL ||
            GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
            StringCchPrintfW(cmd, cch,
                             L"\"%ls\" run \"%ls\" --format json --thinking --title process-risk-analysis",
                             cand, prompt);
            return TRUE;
        }
    }
    return FALSE;
}

/* 提示词：单行、不含引号，避免命令行转义问题 */
void AiBuildPrompt(const ProcInfo *p, WCHAR *buf, size_t cch)
{
    WCHAR ports[512];

    ports[0] = L'\0';
    {
        NetList nl;
        ZeroMemory(&nl, sizeof(nl));
        if (ScanListenPorts(&nl) >= 0) {
            for (size_t i = 0; i < nl.count && lstrlenW(ports) + 24 < 512; i++) {
                if (nl.items[i].pid != p->pid)
                    continue;
                WCHAR one[24];
                StringCchPrintfW(one, 24, L"%lu%ls ",
                                 (unsigned long)nl.items[i].port,
                                 nl.items[i].tcp ? L"/tcp " : L"/udp ");
                StringCchCatW(ports, 512, one);
            }
            FreeNetList(&nl);
        }
    }

    StringCchPrintfW(buf, cch,
        L"你是Windows进程管理专家。分析以下进程并评估终止它的风险，"
        L"用中文分点简洁回答（300字内）："
        L"1)该进程是什么（服务/程序/常见用途）；"
        L"2)终止风险评级：低/中/高；"
        L"3)评级依据（系统关键性、父进程关系、未保存数据丢失、是否会自动重启、"
        L"对监听服务的影响）；4)建议操作。"
        L"进程信息：名称=%ls；PID=%lu；父PID=%lu；内存=%luKB；类型=%ls；路径=%ls；"
        L"监听端口=%ls。",
        p->name, (unsigned long)p->pid, (unsigned long)p->ppid,
        (unsigned long)(p->memBytes >> 10),
        p->type == PT_NODE ? L"Node.js"
                           : (p->type == PT_PYTHON ? L"Python" : L"普通进程"),
        p->path[0] ? p->path : L"(未知)",
        ports[0] ? ports : L"无");
}

/* ---------------- JSON 事件流解析 ---------------- */

typedef struct {
    WCHAR *buf;
    size_t len;
    size_t cap;
} WBuf;

static BOOL WBufAppend(WBuf *b, WCHAR c)
{
    if (b->len + 2 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        WCHAR *t = (WCHAR *)realloc(b->buf, nc * sizeof(WCHAR));
        if (!t)
            return FALSE;
        b->buf = t;
        b->cap = nc;
    }
    b->buf[b->len++] = c;
    b->buf[b->len] = L'\0';
    return TRUE;
}

static void WBufFree(WBuf *b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* 解析 JSON 字符串值（p 指向开引号后的首字符），反转义后追加到 out，
 * 返回值结束位置 */
static const char *ParseJsonValue(const char *p, WBuf *out)
{
    WCHAR wc;

    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': wc = L'\n'; p++; break;
            case 'r': wc = L'\r'; p++; break;
            case 't': wc = L'\t'; p++; break;
            case 'b': wc = L'\b'; p++; break;
            case 'f': wc = L'\f'; p++; break;
            case '/': wc = L'/';  p++; break;
            case '"': wc = L'"';  p++; break;
            case '\\': wc = L'\\'; p++; break;
            case 'u': {
                unsigned v = 0;
                p++;
                for (int k = 0; k < 4 && *p; k++, p++) {
                    char c = *p;
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                    else { v >>= 4; break; }
                }
                if (v >= 0xD800 && v <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = 0;
                    const char *q = p + 2;
                    for (int k = 0; k < 4 && *q; k++, q++) {
                        char c = *q;
                        lo <<= 4;
                        if (c >= '0' && c <= '9') lo |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') lo |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') lo |= (unsigned)(c - 'A' + 10);
                        else { lo >>= 4; break; }
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        wc = (WCHAR)(0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00));
                        p = q;
                        break;
                    }
                }
                wc = (WCHAR)v;
                break;
            }
            default: wc = (WCHAR)(unsigned char)*p; p++; break;
            }
        } else {
            /* UTF-8 多字节还原 */
            int need = 1;
            unsigned cp = (unsigned char)*p;
            if ((cp & 0xE0) == 0xC0) need = 2;
            else if ((cp & 0xF0) == 0xE0) need = 3;
            else if ((cp & 0xF8) == 0xF0) need = 4;
            {
                char tmp[8];
                WCHAR w[3] = { 0, 0, 0 };
                for (int k = 0; k < need && k < 6; k++)
                    tmp[k] = p[k];
                tmp[need < 6 ? need : 6] = '\0';
                MultiByteToWideChar(CP_UTF8, 0, tmp, need, w, 2);
                wc = w[0];
            }
            p += need;
        }
        if (!WBufAppend(out, wc))
            break;
    }
    if (*p == '"')
        p++;
    return p;
}

#define EV_REASONING "\"type\":\"reasoning\""
#define EV_TEXT "\"type\":\"text\""

/* 在事件行中定位 "type":"<wantType>" 之后的 "text":" 值并追加到 out */
static void ExtractEventText(const char *line, const char *wantType, WBuf *out)
{
    const char *t = strstr(line, wantType);
    const char *k;

    if (!t || out->len > AI_WCAP)
        return;
    k = strstr(t, "\"text\":\"");
    if (!k)
        return;
    ParseJsonValue(k + 8, out);
}

/* 遍历 stdout 原始字节中的每一行事件，汇总 thinking / answer；
 * 若整个输出不含事件（kilo 改版/异常），将原文按 UTF-8 兜底为 answer */
static void ParseEventStream(const char *raw, WBuf *thinking, WBuf *answer)
{
    const char *line = raw;
    const char *p = raw;
    BOOL sawEvents = FALSE;

    while (*p || p != line) {
        if (*p == '\n' || !*p) {
            size_t len = (size_t)(p - line);
            char *tmp;

            if (len > 0 && len < 65536) {
                tmp = (char *)malloc(len + 1);
                if (tmp) {
                    memcpy(tmp, line, len);
                    tmp[len] = '\0';
                    if (strstr(tmp, EV_REASONING)) {
                        sawEvents = TRUE;
                        ExtractEventText(tmp, EV_REASONING, thinking);
                    } else if (strstr(tmp, EV_TEXT)) {
                        sawEvents = TRUE;
                        ExtractEventText(tmp, EV_TEXT, answer);
                    }
                    free(tmp);
                }
            }
            if (!*p)
                break;
            line = p + 1;
        }
        p++;
    }

    if (!sawEvents && raw && *raw) {
        /* 无事件结构（kilo 改版/降级）：整段按 UTF-8 兜底为答案 */
        int wlen = MultiByteToWideChar(CP_UTF8, 0, raw, -1, NULL, 0);
        if (wlen > 1 && wlen <= AI_WCAP) {
            if (!answer->buf) {
                answer->cap = (size_t)wlen + 1;
                answer->buf = (WCHAR *)malloc(answer->cap * sizeof(WCHAR));
            } else if (answer->cap < (size_t)wlen) {
                WCHAR *t = (WCHAR *)realloc(answer->buf,
                                            ((size_t)wlen + 1) * sizeof(WCHAR));
                if (t) {
                    answer->buf = t;
                    answer->cap = (size_t)wlen + 1;
                }
            }
            if (answer->buf && answer->cap > (size_t)wlen) {
                MultiByteToWideChar(CP_UTF8, 0, raw, -1, answer->buf, wlen);
                answer->len = (size_t)wlen - 1;
                answer->buf[answer->len] = L'\0';
            }
        }
    }
}

/* ---------------- 进程执行与结果组装 ---------------- */

static void ReadErrFile(const WCHAR *path, WCHAR *buf, size_t cch)
{
    HANDLE f;
    char raw[2048];
    DWORD n = 0;
    int wlen;

    buf[0] = L'\0';
    if (!path || !cch)
        return;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(f, raw, sizeof(raw) - 1, &n, NULL) && n > 0) {
        raw[n] = '\0';
        wlen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)n, NULL, 0);
        if (wlen <= 0)
            wlen = MultiByteToWideChar(CP_ACP, 0, raw, (int)n, NULL, 0);
        if (wlen > 0) {
            if (wlen >= (int)cch)
                wlen = (int)cch - 1;
            MultiByteToWideChar(CP_UTF8, 0, raw, (int)n, buf, wlen);
            buf[wlen] = L'\0';
        }
    }
    CloseHandle(f);
}

static AiResult *AiResultNew(void)
{
    AiResult *r = (AiResult *)malloc(sizeof(AiResult));
    if (r) {
        r->answer = NULL;
        r->thinking = NULL;
        r->diag = NULL;
    }
    return r;
}

void AiResultFree(AiResult *r)
{
    if (!r)
        return;
    free(r->answer);
    free(r->thinking);
    free(r->diag);
    free(r);
}

/* 单次执行 kilo；返回 AiResult（answer 为空表示本次失败，diag 带原因） */
static AiResult *RunKiloOnce(const WCHAR *prompt)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, errFile = INVALID_HANDLE_VALUE;
    AiResult *res;
    char *raw = NULL;
    DWORD rawLen = 0, got;
    WCHAR cmd[8192];
    WCHAR errPath[MAX_PATH + 32];
    WCHAR cwd[MAX_PATH];
    WCHAR errBuf[1280];
    DWORD tmpLen;
    ULONGLONG t0;
    BOOL exited = FALSE;
    WBuf thinking = { NULL, 0, 0 }, answer = { NULL, 0, 0 };

    res = AiResultNew();
    if (!res)
        return NULL;
    errBuf[0] = L'\0';

    tmpLen = GetTempPathW(MAX_PATH, errPath);
    if (tmpLen > 0 && tmpLen < MAX_PATH)
        StringCchCatW(errPath, MAX_PATH + 32, L"kpt-kilo-stderr.txt");
    else
        StringCchCopyW(errPath, MAX_PATH + 32, L"kpt-kilo-stderr.txt");
    DeleteFileW(errPath);

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        StringCchPrintfW(errBuf, 1280, L"CreatePipe 失败（错误 %lu）",
                         (unsigned long)GetLastError());
        goto fail;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    errFile = CreateFileW(errPath, GENERIC_WRITE, FILE_SHARE_READ,
                          &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = errFile != INVALID_HANDLE_VALUE ? errFile : NULL;
    si.hStdInput = NULL;

    if (!BuildKiloCmd(cmd, 8192, prompt)) {
        StringCchCopyW(errBuf, 1280,
                       L"未找到 kilo.exe（可设置环境变量 KILO_EXE 指定路径）");
        goto fail;
    }
    if (GetEnvironmentVariableW(L"USERPROFILE", cwd, MAX_PATH) == 0)
        cwd[0] = L'\0';

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, cwd[0] ? cwd : NULL, &si, &pi)) {
        StringCchPrintfW(errBuf, 1280, L"启动 kilo 失败（错误 %lu）",
                         (unsigned long)GetLastError());
        goto fail;
    }
    CloseHandle(wr);
    wr = NULL;
    if (errFile != INVALID_HANDLE_VALUE) {
        CloseHandle(errFile);
        errFile = INVALID_HANDLE_VALUE;
    }

    raw = (char *)malloc(AI_OUT_CAP);
    if (raw) {
        t0 = GetTickCount64();
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL))
                break;
            if (avail == 0) {
                if (exited)
                    break;
                if (GetTickCount64() - t0 > AI_TIMEOUT_MS) {
                    TerminateProcess(pi.hProcess, 1);
                    StringCchCopyW(errBuf, 1280, L"分析超时（5 分钟）已终止。");
                    break;
                }
                if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0)
                    exited = TRUE;
                continue;
            }
            if (avail > AI_OUT_CAP - 1 - rawLen)
                avail = AI_OUT_CAP - 1 - rawLen;
            if (!ReadFile(rd, raw + rawLen, avail, &got, NULL) || !got)
                break;
            rawLen += got;
        }
        raw[rawLen] = '\0';
        ParseEventStream(raw, &thinking, &answer);
        free(raw);
    }

    {
        DWORD exitCode = 0;
        if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT)
            TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        if (!answer.buf || !answer.buf[0]) {
            WCHAR fileErr[1024];
            ReadErrFile(errPath, fileErr, 1024);
            StringCchPrintfW(errBuf, 1280, L"kilo 退出码 %lu%s%ls",
                             (unsigned long)exitCode,
                             fileErr[0] ? L"，stderr 输出：" : L"（无 stderr 输出）",
                             fileErr);
            goto fail_keep_wbufs;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (wr)
        CloseHandle(wr);
    if (errFile != INVALID_HANDLE_VALUE)
        CloseHandle(errFile);
    DeleteFileW(errPath);

    res->answer = answer.buf;
    res->thinking = thinking.buf;
    return res;

fail_keep_wbufs:
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (wr)
        CloseHandle(wr);
    if (errFile != INVALID_HANDLE_VALUE)
        CloseHandle(errFile);
    DeleteFileW(errPath);
    WBufFree(&answer);
    WBufFree(&thinking);
    res->diag = _wcsdup(errBuf[0] ? errBuf : L"未知原因");
    return res;

fail:
    if (rd)
        CloseHandle(rd);
    if (wr)
        CloseHandle(wr);
    if (errFile != INVALID_HANDLE_VALUE)
        CloseHandle(errFile);
    DeleteFileW(errPath);
    WBufFree(&answer);
    WBufFree(&thinking);
    res->diag = _wcsdup(errBuf[0] ? errBuf : L"未知原因");
    return res;
}

/* 3 次退避重试（3/8/15 秒）：kilo 偶发退出码 3，重试显著提高成功率 */
typedef struct {
    HWND notify;
    WCHAR *prompt;
} AiTask;

static BOOL s_busy = FALSE;

/* ---------------- WP1: Prompt 模板表 ---------------- */

static const WCHAR *const kPrompts[AIPROMPT_COUNT] = {
    /* AIPROMPT_RISK_SINGLE：单进程风险评估（自由 Markdown，现有行为） */
    L"你是Windows进程管理专家。分析以下进程并评估终止它的风险，"
    L"用中文分点简洁回答（300字内）："
    L"1)该进程是什么（服务/程序/常见用途）；"
    L"2)终止风险评级：低/中/高；"
    L"3)评级依据（系统关键性、父进程关系、未保存数据丢失、是否会自动重启、"
    L"对监听服务的影响）；4)建议操作。"
    L"进程信息：%ls。",

    /* AIPROMPT_RISK_BATCH：批量风险分级（严格 JSON，供表格风险列） */
    L"你是Windows进程风险分析专家。对下列每个进程给出终止风险分级。"
    L"必须只输出一个JSON数组，不要任何其他文字，格式：\n"
    L"[{\"pid\":123,\"level\":\"低|中|高|未知\",\"reason\":\"一句话理由\"}]\n"
    L"分级依据：系统关键性（系统进程=高）、是否承载服务/监听端口、"
    L"父进程关系、用户数据丢失风险。进程清单（制表符分隔：PID 名称 内存KB "
    L"类型 命令行 监听端口）：\n%ls",

    /* AIPROMPT_LOG_REVIEW：日志复盘（Markdown 报告） */
    L"你是Node.js/Python开发运维专家。分析以下进程终止审计日志，"
    L"用中文输出Markdown复盘报告，包含三节：\n"
    L"## 高频异常模式（哪些进程/项目反复被终止、孤儿频发）\n"
    L"## 终止失败根因（权限不足/系统保护/进程已退出等分类统计）\n"
    L"## 开发侧改进建议（脚本退出逻辑、IDE配置、清理策略）\n"
    L"仅针对Node/Python开发场景，500字内。日志（制表符分隔："
    L"时间 来源 进程名 PID 结果 路径）：\n%ls",

    /* AIPROMPT_DIAG_GLOBAL：全局诊断（四区块+动作清单） */
    L"你是Windows开发环境诊断专家。基于以下系统快照输出Markdown诊断报告，"
    L"必须包含四节：\n"
    L"## 发现问题清单（每项标注级别：信息/警告/高危）\n"
    L"## 推测根因\n## 处理建议\n"
    L"## 可执行动作\n"
    L"最后另起一行输出动作清单JSON（供工具解析按钮）：\n"
    L"ACTIONS:[{\"action\":\"clean_orphans\"},"
    L"{\"action\":\"fix_winnat\",\"port\":1234},"
    L"{\"action\":\"goto\",\"view\":\"proc\",\"pid\":5678}]\n"
    L"面向Node/Python开发故障。系统快照：\n%ls",

    /* AIPROMPT_AI_QUERY：CLI 自然语言查询（JSON 输出） */
    L"你是Windows进程查询助手。基于以下系统快照回答用户问题。"
    L"必须只输出一个JSON对象，格式：\n"
    L"{\"answer\":\"一句话回答\",\"pids\":[相关PID],\"analysis\":\"分析\"}\n"
    L"用户问题：%ls\n系统快照：\n%ls",

    /* AIPROMPT_PORT_SUGGEST：CLI 端口故障分析 */
    L"你是Windows网络诊断专家。分析端口 %lu 的故障原因。"
    L"必须只输出一个JSON对象：\n"
    L"{\"root_cause\":\"根因\",\"evidence\":\"依据\",\"fix_steps\":[\"步骤1\",\"步骤2\"]}\n"
    L"端口状态与系统上下文：%ls",
};

const WCHAR *AiGetPrompt(AiPromptId id)
{
    if ((int)id < 0 || id >= AIPROMPT_COUNT)
        return kPrompts[AIPROMPT_RISK_SINGLE];
    return kPrompts[id];
}

/* ---------------- WP1: JSON 提取器 ---------------- */

WCHAR *AiExtractJson(const WCHAR *answer)
{
    const WCHAR *start = NULL;
    const WCHAR *p;
    int depth = 0;
    BOOL inStr = FALSE;
    const WCHAR *end = NULL;

    if (!answer)
        return NULL;

    /* 扫描：定位首个 { 或 [ 之后与之配对的闭合位置（字符串感知） */
    for (p = answer; *p; p++) {
        if (inStr) {
            if (*p == L'\\' && p[1]) {
                p++; /* 跳过转义字符 */
                continue;
            }
            if (*p == L'"')
                inStr = FALSE;
            continue;
        }
        if (*p == L'"') {
            inStr = TRUE;
            continue;
        }
        if (*p == L'{' || *p == L'[') {
            if (depth == 0)
                start = p;
            depth++;
        } else if (*p == L'}' || *p == L']') {
            if (depth > 0) {
                depth--;
                if (depth == 0 && start) {
                    end = p;
                    break; /* 首个完整顶层 JSON 已闭合 */
                }
            }
        }
    }
    if (!start || !end || end <= start)
        return NULL;

    {
        size_t n = (size_t)(end - start) + 1;
        WCHAR *out = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
        if (!out)
            return NULL;
        memcpy(out, start, n * sizeof(WCHAR));
        out[n] = L'\0';
        return out;
    }
}

/* ---------------- WP1: 上下文限流 ---------------- */

size_t AiTruncateContext(WCHAR *buf, size_t maxChars)
{
    size_t len;

    if (!buf)
        return 0;
    len = lstrlenW(buf);
    if (len <= maxChars)
        return len;

    /* 在 maxChars 内找最后一个换行，整行截断避免撕裂字段 */
    size_t cut = maxChars;
    while (cut > 0 && buf[cut - 1] != L'\n')
        cut--;
    if (cut == 0)
        cut = maxChars; /* 无换行则硬截 */
    buf[cut] = L'\0';
    StringCchCatW(buf, maxChars + 40, L"\n…（上下文超限已截断）\n");
    return lstrlenW(buf);
}

static DWORD WINAPI AiThreadProc(LPVOID param)
{
    static const DWORD kRetryDelays[] = { 3000, 8000, 15000 };
    AiTask *task = (AiTask *)param;
    HWND notify = task->notify;
    WCHAR *prompt = task->prompt;
    AiResult *res = NULL;
    DWORD code;

    free(task);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0)
            Sleep(kRetryDelays[attempt - 1]);
        res = RunKiloOnce(prompt);
        if (res && res->answer && res->answer[0])
            break;
        AiResultFree(res);
        res = NULL;
    }
    free(prompt);
    if (!res) {
        res = AiResultNew();
        if (res)
            res->diag = _wcsdup(L"kilo 连续 3 次调用失败（已自动退避重试 3/8/15 秒）。");
    }
    code = (res && res->answer && res->answer[0]) ? 1 : 0;
    s_busy = FALSE;
    PostMessageW(notify, WM_APP_AI_DONE, (WPARAM)code, (LPARAM)res);
    return 0;
}

BOOL AiStartAnalysis(HWND hwndNotify, const WCHAR *prompt)
{
    AiTask *task;
    HANDLE th;
    size_t cch;

    if (s_busy || !hwndNotify || !prompt || !prompt[0])
        return FALSE;

    task = (AiTask *)malloc(sizeof(AiTask));
    if (!task)
        return FALSE;
    cch = (size_t)lstrlenW(prompt) + 1;
    task->notify = hwndNotify;
    task->prompt = (WCHAR *)malloc(cch * sizeof(WCHAR));
    if (!task->prompt) {
        free(task);
        return FALSE;
    }
    StringCchCopyW(task->prompt, cch, prompt);

    th = CreateThread(NULL, 0, AiThreadProc, task, 0, NULL);
    if (!th) {
        free(task->prompt);
        free(task);
        return FALSE;
    }
    CloseHandle(th);
    s_busy = TRUE;
    return TRUE;
}

BOOL AiBusy(void)
{
    return s_busy;
}
