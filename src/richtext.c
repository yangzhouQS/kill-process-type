/* richtext.c - Rich Edit 渲染实现：Markdown 子集（加粗/行内代码/标题/列表）
 * → RTF（GBK 字节串，非 GBK 字符回退 \uN），零第三方依赖。
 */
#include "common.h"
#include <richedit.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#include "richtext.h"

/* ---------------- 动态字节缓冲 ---------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} Buf;

static BOOL BufReserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return TRUE;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + extra + 1)
        nc *= 2;
    char *t = (char *)realloc(b->buf, nc);
    if (!t)
        return FALSE;
    b->buf = t;
    b->cap = nc;
    return TRUE;
}

static void BufStr(Buf *b, const char *s)
{
    size_t n = lstrlenA(s);
    if (BufReserve(b, n))
        StringCchCopyA(b->buf + b->len, b->cap - b->len, s), b->len += n;
}

static void BufCh(Buf *b, char c)
{
    if (BufReserve(b, 1)) {
        b->buf[b->len++] = c;
        b->buf[b->len] = '\0';
    }
}

/* 宽字符段转 GBK 追加；RTF 特殊字符转义；不可转换字符回退 \uN? */
static void BufEscW(Buf *b, const WCHAR *s)
{
    char tmp[16];

    for (; s && *s; s++) {
        WCHAR c = *s;
        if (c == L'\\' || c == L'{' || c == L'}') {
            BufCh(b, '\\');
            BufCh(b, (char)c);
            continue;
        }
        if (c == L'\n') {
            BufStr(b, "\\line ");
            continue;
        }
        if (c < 0x80) {
            BufCh(b, (char)c);
            continue;
        }
        {
            int n = WideCharToMultiByte(936, 0, &c, 1, tmp, sizeof(tmp), NULL, NULL);
            if (n > 0) {
                for (int i = 0; i < n; i++)
                    BufCh(b, tmp[i]);
            } else {
                StringCchPrintfA(tmp, 16, "\\u%u?", (unsigned)(unsigned short)c);
                BufStr(b, tmp);
            }
        }
    }
}

/* ---------------- Markdown 子集 → RTF 片段 ----------------
 * 支持：# / ## / ### 标题、**加粗**、`行内代码`、- / * 列表、空行分段。
 */
static void MdInline(Buf *b, const WCHAR *s, size_t len)
{
    size_t i = 0;

    while (i < len) {
        if (s[i] == L'*' && i + 1 < len && s[i + 1] == L'*') {
            size_t j = i + 2;
            while (j + 1 < len && !(s[j] == L'*' && s[j + 1] == L'*'))
                j++;
            if (j + 1 < len) {
                BufStr(b, "{\\b ");
                for (size_t k = i + 2; k < j; k++) {
                    WCHAR one[2] = { s[k], 0 };
                    BufEscW(b, one);
                }
                BufStr(b, "}");
                i = j + 2;
                continue;
            }
        }
        if (s[i] == L'`') {
            size_t j = i + 1;
            while (j < len && s[j] != L'`')
                j++;
            if (j < len) {
                BufStr(b, "{\\f1\\cf2 ");
                for (size_t k = i + 1; k < j; k++) {
                    WCHAR one[2] = { s[k], 0 };
                    BufEscW(b, one);
                }
                BufStr(b, "}");
                i = j + 1;
                continue;
            }
        }
        {
            WCHAR one[2] = { s[i], 0 };
            BufEscW(b, one);
        }
        i++;
    }
}

static void MdLine(Buf *b, const WCHAR *s, size_t len)
{
    int heading = 0;
    BOOL bullet = FALSE;
    size_t start = 0;

    if (len >= 1 && s[0] == L'#') {
        heading = 1;
        if (len >= 2 && s[1] == L'#') {
            heading = 2;
            if (len >= 3 && s[2] == L'#')
                heading = 3;
        }
        start = (size_t)heading;
        while (start < len && s[start] == L' ')
            start++;
    } else if (len >= 2 && (s[0] == L'-' || s[0] == L'*') && s[1] == L' ') {
        bullet = TRUE;
        start = 2;
    }

    BufStr(b, "\\pard\\sa100 ");
    if (heading) {
        static const char *kHeadFmt[] = { "\\b\\fs26 ", "\\b\\fs24 ", "\\b\\fs22 " };
        BufStr(b, kHeadFmt[heading - 1]);
    }
    if (bullet)
        BufStr(b, "\\u8226?  ");
    MdInline(b, s + start, len - start);
    BufStr(b, "\\par\n");
}

static void MdToRtf(Buf *b, const WCHAR *md)
{
    const WCHAR *p = md ? md : L"（空）";
    const WCHAR *line = p;

    while (*p || p != line) {
        if (*p == L'\n' || !*p) {
            size_t len = (size_t)(p - line);
            if (len && line[len - 1] == L'\r')
                len--;
            if (len == 0)
                BufStr(b, "\\pard\\sa60\\par\n");
            else
                MdLine(b, line, len);
            if (!*p)
                break;
            line = p + 1;
        }
        p++;
    }
}

/* ---------------- Rich Edit 控件 ---------------- */

static const WCHAR *LoadRichEditClass(void)
{
    static const WCHAR *cls;

    if (!cls) {
        if (GetModuleHandleW(L"msftedit.dll") || LoadLibraryW(L"msftedit.dll"))
            cls = L"RICHEDIT50W";
        else if (GetModuleHandleW(L"riched20.dll") || LoadLibraryW(L"riched20.dll"))
            cls = L"RichEdit20W";
    }
    return cls;
}

HWND RichTextCreate(HWND parent, int id)
{
    const WCHAR *cls = LoadRichEditClass();
    HWND h;

    if (!cls)
        return NULL;
    h = CreateWindowExW(WS_EX_CLIENTEDGE, cls, NULL,
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                            ES_MULTILINE | ES_READONLY,
                        0, 0, 100, 100, parent, (HMENU)(INT_PTR)id,
                        GetModuleHandleW(NULL), NULL);
    if (h) {
        SendMessageW(h, EM_SETEVENTMASK, 0, 0);
        SendMessageW(h, EM_AUTOURLDETECT, FALSE, 0);
    }
    return h;
}

typedef struct {
    const char *p;
    size_t left;
} StreamCtx;

static DWORD CALLBACK StreamCb(DWORD_PTR cookie, LPBYTE pb, LONG cb, LONG *pcb)
{
    StreamCtx *c = (StreamCtx *)cookie;
    LONG n = c->left < (size_t)cb ? (LONG)c->left : cb;

    memcpy(pb, c->p, (size_t)n);
    c->p += n;
    c->left -= (size_t)n;
    *pcb = n;
    return 0;
}

void RichTextSetRtf(HWND hRich, const char *rtf)
{
    StreamCtx c;
    EDITSTREAM es;

    if (!hRich || !rtf)
        return;
    c.p = rtf;
    c.left = strlen(rtf);
    es.dwCookie = (DWORD_PTR)&c;
    es.dwError = 0;
    es.pfnCallback = StreamCb;
    SendMessageW(hRich, EM_STREAMIN, (WPARAM)SF_RTF, (LPARAM)&es);
}

/* ---------------- AI 报告组装 ---------------- */

static void SectionTitle(Buf *b, const WCHAR *title)
{
    BufStr(b, "\\pard\\sa60\\keep\\cf2\\b\\fs22 ");
    BufEscW(b, title);
    BufStr(b, "\\cf0\\b0\\fs20\\par\n");
}

char *RichTextBuildAiReport(const WCHAR *prompt, const WCHAR *thinking,
                            const WCHAR *answer, const WCHAR *diag)
{
    Buf b = { NULL, 0, 0 };

    BufStr(&b, "{\\rtf1\\ansi\\ansicpg936\\deff0\\deflangfe2052\r\n"
               "{\\fonttbl{\\f0\\fnil\\fcharset134 Microsoft YaHei UI;}"
               "{\\f1\\fmodern\\fcharset134 Consolas;}}\r\n"
               "{\\colortbl ;\\red96\\green96\\blue96;\\red0\\green102\\blue204;"
               "\\red178\\green44\\blue44;}\r\n"
               "\\f0\\fs20\r\n");

    SectionTitle(&b, L"▎提示词");
    BufStr(&b, "\\pard\\li340\\sa120\\cf1\\f1 ");
    {
        /* 提示词为单行长串：分号后换行提升可读性 */
        WCHAR one[2] = { 0, 0 };
        for (const WCHAR *p = prompt ? prompt : L"(无)"; *p; p++) {
            if (*p == L'；') {
                BufEscW(&b, L"；");
                BufStr(&b, "\\line ");
            } else {
                one[0] = *p;
                BufEscW(&b, one);
            }
        }
    }
    BufStr(&b, "\\cf0\\f0\\par\n");

    if (thinking && thinking[0]) {
        SectionTitle(&b, L"▎思考过程");
        BufStr(&b, "\\pard\\li340\\sa120\\cf1\\i ");
        BufEscW(&b, thinking);
        BufStr(&b, "\\i0\\cf0\\par\n");
    }

    if (diag && diag[0]) {
        SectionTitle(&b, L"▎调用失败");
        BufStr(&b, "\\pard\\li340\\sa120\\cf3 ");
        BufEscW(&b, diag);
        BufStr(&b, "\\cf0\\par\n");
    } else {
        SectionTitle(&b, L"▎分析结果");
        BufStr(&b, "\\pard\\sa60 ");
        MdToRtf(&b, answer);
    }

    BufStr(&b, "}\0");
    return b.buf;
}
