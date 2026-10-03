/* PARLZ Editor (pee) -- 自定按键绑定的终端文本编辑器(不模仿 nano / vim)
 *
 * 用法: pee [选项] [文件]
 *   pee 文件名        编辑文件, 不存在则新建
 *   pee +10 文件名    打开并定位到第 10 行
 *   -R 只读   -h 帮助   -V 版本
 *
 * 绑定: Ctrl+S 保存  Ctrl+O 打开  Ctrl+N 新建  Ctrl+Q 退出  F1 帮助
 *       Ctrl+C/X/V 复制/剪切/粘贴(复制内容不含左侧行号), Ctrl+D 删行
 *       Ctrl+F 查找  Ctrl+R 替换  Ctrl+T 跳行  F5 重排段落  Shift+方向键 选择
 *       Ctrl+点击/Ctrl+L 多光标  Alt+H 历史版本(存盘时往 .pee/snap/ 留快照)
 *
 * 单文件纯 C。Windows 用 Console API 读键并对接系统剪贴板(需 -luser32),
 * 类 Unix 用 termios; 输出统一走 ANSI 转义序列, 缓冲区为 UTF-8。
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#define PEE_ISATTY(fd) _isatty(fd)
#define PEE_STAT       _stat
#define PEE_ISDIR(m)   ((m) & _S_IFDIR)
#define PEE_ISREG(m)   ((m) & _S_IFREG)
#define PEE_STAT_T       struct _stat64i32
#define PEE_MKDIR(p)     _mkdir(p)
#else
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <iconv.h>                 /* GBK <-> UTF-8 转换(本机没有 POSIX 编译器, 这部分未编译验证) */
#define PEE_ISATTY(fd) isatty(fd)
#define PEE_STAT       stat
#define PEE_ISDIR(m)   S_ISDIR(m)
#define PEE_ISREG(m)   S_ISREG(m)
#define PEE_STAT_T       struct stat
#define PEE_MKDIR(p)     mkdir(p, 0700)
#endif

#define PEE_NAME "PARLZ Editor"        /* 产品名; 命令名是 pee */
#define PEE_VERSION "1.0"

/* ====================== 键码 ====================== */
enum {
    K_NONE = -1,          /* 输入结束 (EOF) */
    K_UNKNOWN = 0,
    K_TAB = 9,
    K_ENTER = 13,
    K_ESC = 27,
    K_BACKSP = 127,
    K_UP = 0x1000, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_INS, K_DEL,
    K_CLEFT, K_CRIGHT, K_CUP, K_CDOWN, K_CHOME, K_CEND, K_CDEL,
    K_ASSLEFT, K_ASSRIGHT, K_ASSUP, K_ASSDOWN,
    K_PPAGE, K_NPAGE,
    K_SUP, K_SDOWN, K_SLEFT, K_SRIGHT, K_SHOME, K_SEND, K_SPGUP, K_SPGDN,
    K_CSSLEFT, K_CSSRIGHT,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
    K_RESIZE = 0x1FF0, K_MOUSE = 0x1FF1,
    K_META = 0x2000
};

#define CTRL(ch)   (((ch) | 0x20) - 0x60)
#define META(ch)   (K_META | (((ch) | 0x20) - 0x60))
#define IS_META(k) ((k) >= K_META && (k) < K_META + 128)
#define META_CH(k) ((char)(((k) & 0x1F) + 0x60))
#define IS_CTRL(k) ((k) >= 1 && (k) <= 31)

/* ====================== 基础设施 ====================== */
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("pee: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputs("\n", stderr);
    va_end(ap);
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("内存不足");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("内存不足");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

typedef struct { char *b; size_t len, cap; } Str;

static void str_reset(Str *s) { s->len = 0; if (s->b) s->b[0] = 0; }

static void str_reserve(Str *s, size_t extra)
{
    if (s->len + extra + 1 > s->cap) {
        s->cap = (s->len + extra + 1) * 2 + 64;
        s->b = xrealloc(s->b, s->cap);
        s->b[s->len] = 0;
    }
}

static void str_add(Str *s, const char *t, size_t n)
{
    if (!n) return;
    str_reserve(s, n);
    memcpy(s->b + s->len, t, n);
    s->len += n;
    s->b[s->len] = 0;
}

static void str_puts(Str *s, const char *t) { str_add(s, t, strlen(t)); }
static void str_putc(Str *s, char c) { str_add(s, &c, 1); }

static void str_printf_num(Str *s, size_t n)
{
    char b[24];
    snprintf(b, sizeof b, "%zu", n);
    str_puts(s, b);
}

/* ====================== 全局选项 ====================== */
static size_t tabstop = 8;      /* -t / --tabsize */
static int autoindent = 1;      /* --no-autoindent 关掉 */
static int overwrite;           /* Ins 键切换插入/覆盖 */
static int use_regex;            /* 搜索提示里 Alt+R 切换 */
static int do_backup;             /* -B / --backup: 保存前留 name~ */
static int fillcol = 72;            /* set fill N: 重排宽度, 0=按屏幕宽 */
static int positionlog = 1;         /* set positionlog off 关掉光标位置记忆 */
static int force_bom;              /* --bom / set bom on: 保存时写 UTF-8 BOM */
static int allow_exec = 1;        /* F8/F9 外部命令; --no-execute / set execute off 关掉 */
static int snapkeep = 20;         /* 存盘时往 同目录/.pee/snap/ 留一份; set snapkeep N, 0 或 --no-snap 关掉 */
static int start_history;            /* --history: 打开文件后直接进版本列表 */
enum { ENC_UTF8 = 0, ENC_GBK = 1 };   /* 磁盘编码; 缓冲区内部永远是 UTF-8 */
static int force_enc = -1;        /* --encoding / set encoding: -1 = 自动检测 */
static char *speller_cmd;          /* set speller "命令": 拼写检查走外部程序 */
static char *speller_ok;           /* 试探出来的可用命令 */
static int rect_paste_mode;   /* 上次是列块操作 -> 粘贴也按列 */
static int rect_sel;                 /* Alt+Shift+方向: 列块选择 */
static int spot_on;                  /* 逐个确认替换中: 命中处用 spotlightcolor */
static int nmc, mc_depth;            /* 多光标: 额外光标个数 / 正在把一次编辑铺到各光标 */
#define MC_MAX 64
static int msg_err;                  /* 当前状态消息是错误(走 errorcolor) */
static int softwrap;              /* Alt+W / --softwrap: 长行按屏幕宽度折显 */
static int smarthome = 1;          /* set smarthome off: Home 只到第 0 列(默认在行首与首个非空白间跳) */
static int cutfromcursor;          /* set cutfromcursor on: 无选区时 Ctrl+X 只剪光标到行尾 */
static int casesens = 1;           /* set casesensitive off: 查询串全小写时忽略大小写 */
static int showblanks;             /* set showblanks on: 制表符与行尾空格可视化 */
static int nonewlines;             /* set nonewlines on: 文末不补换行 */
static int atblanks;               /* set atblanks on: 软换行在空白处断行 */
static const char *ws_tab = "\xC2\xB7";    /* 制表符占位符 */
static const char *ws_sp = "\xC2\xB7";     /* 行尾空格占位符 */
static const char *match_pairs = "()[]{}<>";
static int bmark_on;               /* 有没有书签 */
static int hilite_on = 1;          /* set syntax off 关掉高亮 */
static unsigned char *hl_mask;     /* 每字节一个"规则号+1", HL_BLOCK=块注释延续 */
static size_t hl_maskcap, hl_len;
#define HL_NONE 0
#define HL_BLOCK 255               /* 块注释跨行延续的着色槽 */
/* --- 界面配色槽: 值与 uisgr_dflt 一一对应, set xxxcolor 可改 --- */
enum { UC_TITLE, UC_BAR, UC_STATUS, UC_PROMPT, UC_SEL, UC_SPOT, UC_ERR,
       UC_DIRO, UC_DIRN, UC_N };
static char uisgr[UC_N][32];
static void cm_update(size_t upto);        /* 跨行块注释状态, 每帧推到视口底 */
/* 最近一次鼠标事件 */
static int ms_row, ms_col, ms_btn, ms_press;   /* 0 基屏幕行列, 按钮号, 按下/抬起 */
static int ms_ctrl;                            /* 这次鼠标事件按着 Ctrl 吗 */

/* 软换行时: 每个显示行 -> (缓冲区行, 起始列) */
static size_t *map_line, *map_col, *line_row0;
static size_t map_cap, row_total;
static void (*prompt_notify)(const char *);


/* ====================== UTF-8 ====================== */
static int u8_seqlen(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

static uint32_t u8_decode(const char *p, size_t avail, int *len)
{
    unsigned char c = (unsigned char)*p;
    int k = u8_seqlen(c);
    if ((size_t)k > avail) { *len = 1; return c; }
    for (int i = 1; i < k; i++)
        if (((unsigned char)p[i] & 0xC0) != 0x80) { *len = 1; return c; }
    uint32_t cp = c & (0x7F >> (k - 1));
    for (int i = 1; i < k; i++) cp = (cp << 6) | ((unsigned char)p[i] & 0x3F);
    *len = k;
    return cp;
}

static int u8_width(uint32_t cp)
{
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (cp == 0xFE0F || cp == 0x200D || cp == 0x200B) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xE000 && cp <= 0xE0FF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

/* 一个字节的宽度(用于遍历): 组合字符算 0 但至少要占位的情况由调用者处理 */
static size_t cellw(uint32_t cp, size_t col)
{
    if (cp == '\t') return tabstop - (col % tabstop);
    return (size_t)u8_width(cp);
}

static size_t str_cells(const char *s)
{
    size_t cols = 0;
    while (*s) {
        int len;
        uint32_t cp = u8_decode(s, strlen(s), &len);
        cols += cellw(cp, cols);
        s += len;
    }
    return cols;
}

/* ====================== 文档 ====================== */
typedef struct { char *b; size_t len, cap; } Line;
typedef struct { size_t line, off; } Pos;

struct Uop;                       /* 增量撤销的一步, 定义在撤销小节 */
typedef struct Uop Uop;
static Uop *rec_op;               /* 当前这一步正在收进来的改动记到这儿 */

typedef struct {
    Line *v;
    size_t n, cap;
    Pos cur;
    Pos mark;               /* mark.line == (size_t)-1 表示未设置 */
    size_t top;             /* 视口首行 */
    size_t sc;              /* 水平滚动(列) */
    size_t curow;           /* 上下移动时记住的列 */
    int modified;
    int crlf;
    int bom;      /* 文件带 UTF-8 BOM: 读写都保留, 不进正文 */
    int enc;      /* 磁盘编码: ENC_UTF8 / ENC_GBK(缓冲区里永远存 UTF-8) */
    int readonly;
    int hist;               /* 内容来自历史快照: 存盘等于另存, 且不再产生快照 */
    char *fname;
    /* 撤销/重做按缓冲各存一份(定义在后面的撤销小节) */
    Uop *ustack, *rstack;
    size_t un, rn, ucap, rcap, umem, rmem;
    int batch_kind;
} Doc;

typedef struct {
    int w, h;
    int tty;
    int rawpipe;
    Str frame, prev;
    int full;
#ifdef _WIN32
    HANDLE hin, hout;
    DWORD oldin, oldout;
    int haveoldin, haveoutout;
    BYTE *inbuf;
    size_t inlen, inpos;
#else
    struct termios orig;
    int origok;
#endif
} Term;

static Doc D;
#define MAXBUFS 16
static Doc bufs[MAXBUFS];
static int nbufs, curbuf;
static Term T;
static const char **pg_lines;      /* 覆盖层现在显示哪一组行(帮助 / 历史版本列表) */
static int pg_n;
static Str cutbuf, searchbuf, msgbuf;
static Pos search_anchor;
static int help_mode;
static int quitting;

/* 前向声明 */
static void doc_free(void);
static void insert_raw(const char *data, size_t n);
static void insert_newline(void);
static void cut_range(Pos a, Pos b, int to_cut);
static void delete_selection(void);
static void select_all(void);
static void mc_toggle_at(Pos q);
static int mc_cell_in(int n, const size_t *s0, const size_t *s1, size_t cell);
static int mc_row_cells(size_t li, size_t *cols, size_t *s0, size_t *s1, int max);

static void go_line(size_t n);
static int prompt(Term *t, const char *label, char *buf, size_t size,
                  const char *init, int *cancelled);
static void do_writeout(Term *t, int append_mode);
static void term_init(Term *t);
static void term_restore(Term *t);
static void term_size(Term *t);
static void term_write(Term *t, const char *s, size_t n);
static int term_read_key(Term *t, char *utf8, uint32_t *cp_out);

static void set_msg(const char *fmt, ...);

/* --- 宏: F6 录制/停止, F7 回放最近一个 --- */
typedef struct { int k; char u[8]; } MKey;
#define MACRO_MAX 8
#define MACRO_KEYS 4096
static MKey macros[MACRO_MAX][MACRO_KEYS];
static size_t macro_len[MACRO_MAX];
static int macro_n, recording;
static MKey rbuf[MACRO_KEYS];
static size_t rq_head, rq_tail;
static long replay_left;

static int read_key(Term *t, char *utf8, uint32_t *cp_out)
{
    if (replay_left > 0 && rq_head != rq_tail) {
        MKey m = rbuf[rq_head++];
        if (rq_head >= MACRO_KEYS) rq_head = 0;
        strcpy(utf8, m.u);
        int l;
        *cp_out = u8_decode(m.u, strlen(m.u), &l);
        if (--replay_left <= 0) { rq_head = rq_tail = 0; }
        return m.k;
    }
    if (replay_left > 0) replay_left = 0;
    int k = term_read_key(t, utf8, cp_out);
    /* F6 本身不进宏: 否则回放时会把录制又打开 */
    if (recording && k != K_NONE && k != K_F6) {
        int slot = macro_n ? macro_n - 1 : 0;
        if (macro_n == 0) { macro_n = 1; }
        if (macro_len[slot] < MACRO_KEYS) {
            macros[slot][macro_len[slot]].k = k;
            strcpy(macros[slot][macro_len[slot]].u, utf8);
            macro_len[slot]++;
        }
    }
    return k;
}

static void macro_toggle_record(void)
{
    if (!recording) {
        if (macro_n < MACRO_MAX) macro_n++;
        if (macro_n > MACRO_MAX) macro_n = MACRO_MAX;
        macro_len[macro_n - 1] = 0;
        recording = 1;
        set_msg("开始录制宏 #%d", macro_n);
    } else {
        recording = 0;
        set_msg("录制结束: 宏 #%d 共 %zu 键", macro_n, macro_len[macro_n - 1]);
    }
}

static void macro_replay(void)
{
    if (!macro_n) { set_msg("还没有录制宏"); return; }
    if (recording) { set_msg("正在录制, 先按 F6 停止"); return; }
    int slot = macro_n - 1;
    if (!macro_len[slot]) { set_msg("这个宏是空的"); return; }
    rq_head = rq_tail = 0;
    for (size_t i = 0; i < macro_len[slot]; i++) {
        rbuf[rq_tail] = macros[slot][i];
        if (++rq_tail >= MACRO_KEYS) rq_tail = 0;
        if (rq_tail == rq_head) { set_msg("宏太长, 回放截断"); break; }
    }
    replay_left = (long)macro_len[slot];
}
static size_t str_cells(const char *s);
static int page_overlay(Term *t, int any_key, char *gotch);
static void do_history(Term *t);
static void buf_store(void);
static void buf_select(Term *t, int i);
static void buf_switch(Term *t, int dir);
static void buf_newbuf(Term *t);
static void buf_close(Term *t);
static void set_msg(const char *fmt, ...);
static void set_err(const char *fmt, ...);
static void inc_notify(const char *text);
static int rect_bounds(size_t *l0, size_t *l1, size_t *c0, size_t *c1);
static void pos_remember(const char *name, long line, long col);
static int pos_recall(const char *name, long *line, long *col);
static void hl_line(const Line *l, size_t li);
static const char *hl_sgr(int cls);
static int ask_key(Term *t, const char *question);
static void hl_pick(const char *fname);
static char *read_file(const char *path, size_t *len, int *err);
static size_t cellw(uint32_t cp, size_t col);
static int enc_parse(const char *v, int *out);
static const char *enc_name(int e);

/* 原地跳过头部的 UTF-8 BOM(记事本/VSCode 存出来的配置文件常带) */
static void skip_bom(char **p, size_t *n)
{
    if (*n >= 3 && (unsigned char)(*p)[0] == 0xEF &&
        (unsigned char)(*p)[1] == 0xBB && (unsigned char)(*p)[2] == 0xBF) {
        *p += 3;
        *n -= 3;
    }
}

static Pos P(size_t line, size_t off)
{
    Pos p;
    p.line = line;
    p.off = off;
    return p;
}

static Pos NO_MARK(void) { return P((size_t)-1, 0); }
static int has_mark(void) { return D.mark.line != (size_t)-1; }

static int pos_less(Pos a, Pos b)
{
    return a.line < b.line || (a.line == b.line && a.off < b.off);
}

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    str_reset(&msgbuf);
    str_puts(&msgbuf, buf);
    msg_err = 0;
}

/* 同样的消息, 但用 errorcolor 显示 */
static void set_err(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    str_reset(&msgbuf);
    str_puts(&msgbuf, buf);
    msg_err = 1;
}

/* ====================== 行缓冲 ====================== */
static void line_set(Line *l, const char *data, size_t n)
{
    l->b = xmalloc(n + 1);
    memcpy(l->b, data, n);
    l->b[n] = 0;
    l->len = n;
    l->cap = n + 1;
}

static void line_free(Line *l)
{
    free(l->b);
    l->b = NULL;
    l->len = l->cap = 0;
}

/* 撤销记录: 任何改动行缓冲字节的动作都要经过 line_insert/line_delete, 在这里被记账 */
static void rec_text(int dir, const Line *l, size_t off, const char *b, size_t n);
static void rec_lines(int dir, size_t i, size_t k);
static void seal_op(void);
static void doc_free_undo(Doc *d);

static void line_insert(Line *l, size_t off, const char *data, size_t n)
{
    if (off > l->len) off = l->len;
    rec_text(1, l, off, data, n);
    if (l->len + n + 1 > l->cap) {
        l->cap = (l->len + n + 1) * 2 + 32;
        l->b = xrealloc(l->b, l->cap);
    }
    memmove(l->b + off + n, l->b + off, l->len - off);
    memcpy(l->b + off, data, n);
    l->len += n;
    l->b[l->len] = 0;
}

static void line_delete(Line *l, size_t off, size_t n)
{
    if (off >= l->len || !n) return;
    if (off + n > l->len) n = l->len - off;
    rec_text(-1, l, off, l->b + off, n);
    memmove(l->b + off, l->b + off + n, l->len - off - n);
    l->len -= n;
    l->b[l->len] = 0;
}

/* 行内 [0,off) 的显示列宽 */
static size_t line_cells(const Line *l, size_t off)
{
    size_t cols = 0, i = 0;
    if (off > l->len) off = l->len;
    while (i < off) {
        int len;
        uint32_t cp = u8_decode(l->b + i, off - i, &len);
        cols += cellw(cp, cols);
        i += len;
    }
    return cols;
}

/* 显示列 -> 字节偏移(不会落在字符中间) */
static size_t line_col_to_off(const Line *l, size_t target)
{
    size_t cols = 0, i = 0;
    while (i < l->len) {
        int len;
        uint32_t cp = u8_decode(l->b + i, l->len - i, &len);
        size_t w = cellw(cp, cols);
        if (!w) w = 1;
        if (cols + w > target) break;
        cols += w;
        i += len;
    }
    return i;
}

static void doc_init(const char *fname, int readonly)
{
    D.cap = 16;
    D.v = xmalloc(sizeof(Line) * D.cap);
    D.n = 1;
    line_set(&D.v[0], "", 0);
    D.cur = P(0, 0);
    D.mark = NO_MARK();
    D.top = 0;
    D.sc = 0;
    D.curow = 0;
    D.modified = 0;
    D.crlf = 0;
    D.bom = force_bom;
    D.enc = force_enc >= 0 ? force_enc : ENC_UTF8;
    D.readonly = readonly;
    D.fname = (fname && *fname) ? xstrdup(fname) : NULL;
}

static void doc_ensure(size_t need)
{
    if (need <= D.cap) return;
    if (!D.cap) D.cap = 16;              /* 否则 0*2 永远追不上 need */
    while (D.cap < need) D.cap *= 2;
    D.v = xrealloc(D.v, sizeof(Line) * D.cap);
}

static void doc_free(void)
{
    seal_op();
    if (nbufs > 0) bufs[curbuf] = D;
    for (int b = 0; b < nbufs; b++) {
        for (size_t i = 0; i < bufs[b].n; i++) line_free(&bufs[b].v[i]);
        free(bufs[b].v);
        free(bufs[b].fname);
        doc_free_undo(&bufs[b]);
    }
    memset(bufs, 0, sizeof bufs);
    memset(&D, 0, sizeof D);
    free(cutbuf.b);
    free(searchbuf.b);
    free(msgbuf.b);
    free(T.frame.b);
    free(T.prev.b);
    memset(&D, 0, sizeof D);
}

static void clamp_cursor(void)
{
    if (D.n == 0) return;
    if (D.cur.line >= D.n) D.cur.line = D.n - 1;
    Line *l = &D.v[D.cur.line];
    if (D.cur.off > l->len) D.cur.off = l->len;
    while (D.cur.off > 0 && ((unsigned char)l->b[D.cur.off] & 0xC0) == 0x80)
        D.cur.off--;
}

/* ====================== 平台层 ====================== */
static int help_ofs;
static const char *help_lines[] = {
    "  " PEE_NAME " " PEE_VERSION " 快捷键(命令名 pee; 自定义方案, 不是 nano/vim)",
        "",
        "  文件",
        "    F1 帮助     Ctrl+S 保存     Ctrl+O 打开     Ctrl+N 新建     Ctrl+Q 退出",
        "  编辑",
        "    Ctrl+C 复制选区   Ctrl+X 剪切选区   Ctrl+V 粘贴   Ctrl+D 删除当前行",
        "    Ctrl+A 全选       Del/Backspace 删字符        Enter 换行   Tab 制表符",
        "    Ctrl+T 跳到行号   F5 重排当前段落             Esc 取消选区或提示",
    "    Ctrl+Z 撤销         Ctrl+Y 重做(连续打字算一步)",
    "    M-L 小写   M-U 大写   M-C 词首大写   Ins 插入/覆盖模式",
    "    M-{ 缩进一级   M-} 反缩进   M-T 行首制表符/空格互换",
    "    F6 录制/停止宏   F7 回放最近一个宏(录制中的按键不被再次记录)",
    "    Ctrl+N 新建缓冲  Ctrl+W 关闭缓冲  M-, / M-. 上/下一个缓冲",
    "    M-I 把另一个文件读入到光标处(缓冲切换会清空撤销历史)",
    "    F8 运行命令并把输出插到光标处  F9 用命令输出替换选区(%f=文件名)",
    "    M-P 拼写检查: 默认用 aspell/hunspell, 也可 set speller \"命令\"",
    "    F12 词补全(连按换下一个)   M-V 插入 Unicode 码点   M-M 跳到配对括号",
    "    M-K 设/清书签   M-J 跳到书签",
    "    多光标: Ctrl+点击 加/去光标   Ctrl+L 选中全文同名词   打字/退格/粘贴同时落到每个光标   Esc 收起",
    "    (多光标时 Ctrl+C/X 把各处选区用换行拼接; 一次按键仍算一步撤销)",
    "    M-O 切换存盘编码(GBK <-> UTF-8; 缓冲区不动, 保存时按这个写)",
    "    M-H 历史版本列表: 每次存盘往 同目录/.pee/snap/ 留一份带时间戳的副本,",
    "       挑一个就读进新缓冲(挂原文件名, 存盘才会覆盖现在的内容)",
    "       pee --history 文件 就是打开后直接进这个列表; set snapkeep 0 关掉存档",
    "    pee -d 甲 乙 两文件并排对照: 左红右绿标出差异, n/p 跳下一处/上一处,",
    "       空格翻页, q 回到编辑左边那个文件, Ctrl+Q 退出(配色 set diffoldcolor/diffnewcolor)",
    "       命令框里 %%f 才是字面 %f; --no-execute 或 set execute off 可禁用",
    "    M-W 软换行开关(长行折显, 此时不做横向滚动)",
    "    鼠标: 单击定位, 拖动选择, 滚轮翻页",
    "  配置文件(pee.cfg / ~/.peerc / %APPDATA%\\pee.cfg):",
    "    set tabsize 4    set autoindent off    set softwrap on    set backup on",
    "    set fill 72      set positionlog off   set bom on / set bom off",
    "    set syntax off   set regexp on",
    "    set smarthome off / cutfromcursor on / casesensitive off / showblanks on / atblanks on",
    "    set nonewlines on / set whitespace \"▸·\" / set matchbrackets \"()[]{}<>\"",
    "    set encoding gbk|utf-8|auto   (默认自动检测; 缓冲区内部永远是 UTF-8)",
    "    set snapkeep 20  每次存盘留的历史副本个数(0 = 关掉时间旅行)",
    "    配色 set titlecolor red / set selectedcolor white,blue / set statuscolor ...",
    "         还有 promptcolor functioncolor spotlightcolor errorcolor",
    "         颜色名 black..white 与 bright*, 可叠 bold italic underline reverse blink",
    "    高亮 syntax \"c\" \"\\.c$\"  里面: color 名 \"正则\" | icolor 名 \"正则\" | comment \"/*\" \"*/\"",
    "         commentcolor 名 ... end syntax  (用户块优先; 块外的 color 适用所有文件)",
    "    bind Ctrl+K Ctrl+X    unbind Ctrl+Q    (键名: Ctrl+/Alt+/Shift+ 组合, F1..F10, Ins)",
        "  查找",
        "    Ctrl+F 查找(增量) Ctrl+R 替换全部   M-E 逐个确认替换(y/n/a/c/q)",
    "    正则替换里 \\1..\\9 引用捕获组; 搜索/文件名框内上下键翻历史(.pee_history)",
    "    搜索框里按 Alt+R 切换正则模式(. * + ? | [] ^ $ \\d \\w)",
        "  移动与选择",
        "    方向键移动  Home/End 行首行尾  PgUp/PgDn 翻页  Ctrl+PgUp/PgDn 只滚动",
        "    Shift+方向键 选择   Ctrl+方向键 按词移动   Ctrl+Shift+方向键 按词选择",
        "    Ctrl+Home/End 到文件首/尾   Ctrl+上/下 视口滚动",
        "  说明",
        "    正文不带行号栏, 复制/剪切/粘贴取的就是正文本身。",
        "    Windows 下复制/粘贴与系统剪贴板互通(类 Unix 用编辑器内部剪贴板)。",
        "",
    "  按任意键返回编辑。",
};

static void draw_screen(Term *t);

#ifdef _WIN32

static const struct { const char *s; int k; } esctab[] = {
    { "[A", K_UP }, { "[B", K_DOWN }, { "[C", K_RIGHT }, { "[D", K_LEFT },
    { "[H", K_HOME }, { "[F", K_END }, { "[1~", K_HOME }, { "[4~", K_END },
    { "[2~", K_INS }, { "[3~", K_DEL }, { "[5~", K_PGUP }, { "[6~", K_PGDN },
    { "[7~", K_HOME }, { "[8~", K_END },
    { "[1;2A", K_SUP }, { "[1;2B", K_SDOWN }, { "[1;2C", K_SRIGHT }, { "[1;2D", K_SLEFT },
    { "[1;2H", K_SHOME }, { "[1;2F", K_SEND },
    { "[1;5C", K_CRIGHT }, { "[1;5D", K_CLEFT }, { "[1;5A", K_CUP }, { "[1;5B", K_CDOWN },
    { "[1;6C", K_CSSRIGHT }, { "[1;6D", K_CSSLEFT },
    { "[1;4C", K_ASSRIGHT }, { "[1;4D", K_ASSLEFT },
    { "[1;4A", K_ASSUP }, { "[1;4B", K_ASSDOWN },
    { "[1;5H", K_CHOME }, { "[1;5F", K_CEND }, { "[3;5~", K_CDEL },
    { "[5;5~", K_CUP }, { "[6;5~", K_CDOWN },
    { "OA", K_UP }, { "OB", K_DOWN }, { "OC", K_RIGHT }, { "OD", K_LEFT },
    { "OH", K_HOME }, { "OF", K_END },
    { "OP", K_F1 }, { "OQ", K_F2 }, { "OR", K_F3 }, { "OS", K_F4 },
    { "[11~", K_F1 }, { "[12~", K_F2 }, { "[13~", K_F3 }, { "[14~", K_F4 },
    { "[15~", K_F5 }, { "[17~", K_F6 }, { "[18~", K_F7 }, { "[19~", K_F8 },
    { "[20~", K_F9 }, { "[21~", K_F10 }, { "[23~", K_F11 }, { "[24~", K_F12 },
};

static int decode_seq(const char *seq, int n)
{
    for (size_t i = 0; i < sizeof esctab / sizeof esctab[0]; i++) {
        size_t l = strlen(esctab[i].s);
        if (l == (size_t)n && !memcmp(esctab[i].s, seq, n)) return esctab[i].k;
    }
    return K_UNKNOWN;
}

static void term_init(Term *t)
{
    t->hout = GetStdHandle(STD_OUTPUT_HANDLE);
    t->hin = GetStdHandle(STD_INPUT_HANDLE);
    t->full = 1;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    t->tty = t->hout && t->hout != INVALID_HANDLE_VALUE &&
             GetConsoleScreenBufferInfo(t->hout, &csbi);
    if (t->tty) {
        t->w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        t->h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCtrlHandler(NULL, TRUE);
        DWORD om = 0;
        if (GetConsoleMode(t->hout, &om)) {
            t->oldout = om;
            t->haveoutout = 1;
            /* 经典 conhost 要显式打开 VT, 否则转义序列会被当字符打出来 */
            SetConsoleMode(t->hout, om | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    if (!t->tty) {
        const char *c = getenv("COLUMNS"), *r = getenv("LINES");
        if (c && *c) t->w = atoi(c);
        if (r && *r) t->h = atoi(r);
    }
    DWORD mode;
    if (t->hin && GetConsoleMode(t->hin, &mode)) {
        t->oldin = mode;
        t->haveoldin = 1;
        SetConsoleMode(t->hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT);
    } else {
        t->rawpipe = 1;
        t->tty = 1;
        size_t cap = 4096, len = 0;
        BYTE *b = xmalloc(cap);
        for (;;) {
            if (len + 1024 > cap) { cap *= 2; b = xrealloc(b, cap); }
            DWORD r = 0;
            if (!ReadFile(t->hin, b + len, (DWORD)(cap - len), &r, NULL) || !r) break;
            len += r;
        }
        t->inbuf = b;
        t->inlen = len;
    }
    if (t->w < 24) t->w = 24;
    if (t->h < 10) t->h = 10;
    if (t->tty && !t->rawpipe) {
        term_write(t, "\x1b[?1049h", 8);
        term_write(t, "\x1b[?1003h\x1b[?1006h", 17);
    }
}

static void term_restore(Term *t)
{
    if (t->tty && !t->rawpipe) term_write(t, "\x1b[0m\x1b[?1049l", 12);
    if (t->haveoldin) SetConsoleMode(t->hin, t->oldin);
    if (t->haveoutout) SetConsoleMode(t->hout, t->oldout);
    free(t->inbuf);
    t->inbuf = NULL;
}

static void term_size(Term *t)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(t->hout, &csbi)) return;
    int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (w < 24) w = 24;
    if (h < 10) h = 10;
    if (w != t->w || h != t->h) { t->w = w; t->h = h; t->full = 1; }
}

static void term_write(Term *t, const char *s, size_t n)
{
    if (t->tty) {
        DWORD done;
        WriteFile(t->hout, s, (DWORD)n, &done, NULL);
    } else {
        fwrite(s, 1, n, stdout);
    }
}

static int pipe_b(Term *t)
{
    return t->inpos < t->inlen ? t->inbuf[t->inpos++] : -1;
}

static int pipe_peek(Term *t)
{
    return t->inpos < t->inlen ? t->inbuf[t->inpos] : -1;
}

/* \x1b[<b;x;yM  ->  K_MOUSE */
static int parse_sgr_mouse(Term *t)
{
    int b = 0, row = 0, col = 0, f;
    int x;
    while ((x = pipe_b(t)) >= '0' && x <= '9') b = b * 10 + (x - '0');
    while (x == ';') { int v = 0;
        while ((x = pipe_b(t)) >= '0' && x <= '9') v = v * 10 + (x - '0');
        if (col == 0) col = v; else row = v;
        if (x != ';') break;
    }
    f = x;
    ms_btn = b & 3;
    if (b & 64) ms_btn = (b & 1) ? 5 : 4;
    ms_press = (f == 'm') ? 0 : ((b & 32) ? 2 : 1);
    ms_ctrl = (b & 16) ? 1 : 0;                  /* SGR 里 0x10 = Ctrl 按住 */
    ms_row = row - 1;
    ms_col = col - 1;
    if (f == 'M' || f == 'm') return K_MOUSE;
    return K_UNKNOWN;
}

/* 系统剪贴板: 读写的都是纯正文, 不含行号栏 */
static void clipboard_put(const char *utf8, size_t n)
{
    int try;
    if (!utf8 || !n) return;
    for (try = 0; try < 12; try++) {                 /* 别的程序正占着剪贴板时重试一会儿 */
        if (OpenClipboard(NULL)) break;
        Sleep(25);
    }
    if (try >= 12) return;
    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, NULL, 0);
    if (wl > 0) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)wl + 1) * sizeof(WCHAR));
        if (h) {
            WCHAR *p = GlobalLock(h);
            if (p) {
                MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, p, wl);
                p[wl] = 0;
                GlobalUnlock(h);
                EmptyClipboard();
                if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            } else {
                GlobalFree(h);
            }
        }
    }
    CloseClipboard();
}

static char *clipboard_get(void)
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL)) return NULL;
    char *out = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        WCHAR *p = GlobalLock(h);
        if (p) {
            int n = (int)lstrlenW(p);
            int need = WideCharToMultiByte(CP_UTF8, 0, p, n, NULL, 0, NULL, NULL);
            if (need > 0) {
                out = xmalloc((size_t)need + 1);
                WideCharToMultiByte(CP_UTF8, 0, p, n, out, need, NULL, NULL);
                out[need] = 0;
                if (!out[0]) { free(out); out = NULL; }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

static int term_read_key(Term *t, char *utf8, uint32_t *cp_out)
{
    utf8[0] = 0;
    *cp_out = 0;
    if (t->rawpipe) {
        int c = pipe_b(t);
        if (c < 0) return K_NONE;
        if (c == 27) {
            int x = pipe_b(t);
            if (x < 0) return K_ESC;
            /* ESC 后面跟控制码: 那是另一个键, 把 ESC 单独交回去 */
            if (x < 32 && x != 27) { t->inpos--; return K_ESC; }
            char seq[8];
            int n = 0;
            seq[n++] = (char)x;
            if (x == '[' && pipe_peek(t) == '<') {
                pipe_b(t);
                return parse_sgr_mouse(t);
            }
            if (x == '[' || x == 'O') {
                while (n < 7) {
                    x = pipe_b(t);
                    if (x < 0) break;
                    seq[n++] = (char)x;
                    if (x >= 0x40 && x <= 0x7E) break;
                }
                return decode_seq(seq, n);
            }
            return META(x);
        }
        if (c == '\n' || c == '\r') { utf8[0] = (char)K_ENTER; return K_ENTER; }
        if (c == '\t') { utf8[0] = (char)K_TAB; return K_TAB; }
        if (c == 8) { utf8[0] = 8; return CTRL('h'); }
        if (c == 127) { utf8[0] = 127; utf8[1] = 0; return K_BACKSP; }
        if (c < 32) return c;
        if (c < 0x80) { utf8[0] = (char)c; utf8[1] = 0; *cp_out = (uint32_t)c; return c; }
        char seq[4];
        int need = u8_seqlen((unsigned char)c);
        seq[0] = (char)c;
        for (int i = 1; i < need; i++) { int x = pipe_b(t); seq[i] = x < 0 ? '?' : (char)x; }
        memcpy(utf8, seq, need);
        utf8[need] = 0;
        int l;
        *cp_out = u8_decode(seq, need, &l);
        return K_UNKNOWN;         /* 非 ASCII 文本走 cp_out/utf8 */
    }

    INPUT_RECORD rec;
    DWORD got;
    for (;;) {
        if (!ReadConsoleInputW(t->hin, &rec, 1, &got) || got != 1) return K_NONE;
        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) return K_RESIZE;
        if (rec.EventType == MOUSE_EVENT) {
            MOUSE_EVENT_RECORD *m = &rec.Event.MouseEvent;
            int moved = (m->dwEventFlags & MOUSE_MOVED) != 0;
            int down = (m->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            if (m->dwEventFlags & MOUSE_WHEELED) {
                ms_btn = ((short)HIWORD(m->dwButtonState) > 0) ? 4 : 5;
                ms_press = 1;
                ms_row = m->dwMousePosition.Y;
                ms_col = m->dwMousePosition.X;
                return K_MOUSE;
            }
            ms_btn = down ? 0 : -1;
            if (moved && !down) continue;               /* 没按着的鼠标移动忽略 */
            ms_press = (moved && down) ? 2 : (down ? 1 : 0);
            ms_ctrl = (m->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) ? 1 : 0;
            ms_row = m->dwMousePosition.Y;
            ms_col = m->dwMousePosition.X;
            if (ms_press == 0) continue;                /* 抬起不动作 */
            return K_MOUSE;
        }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        KEY_EVENT_RECORD *k = &rec.Event.KeyEvent;
        int ctrl = !!(k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
        int alt = !!(k->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
        int shift = !!(k->dwControlKeyState & SHIFT_PRESSED);
        WORD vk = k->wVirtualKeyCode;
        WCHAR uc = k->uChar.UnicodeChar;

        if (vk >= 'A' && vk <= 'Z') {
            if (ctrl) return CTRL((char)vk);
            if (alt) return META((char)vk);
            if (uc) { utf8[0] = (char)uc; utf8[1] = 0; *cp_out = uc; return uc; }
            return K_UNKNOWN;
        }
        if (vk == VK_SPACE) {
            if (ctrl) return CTRL('2');
            if (alt) return META(' ');
            utf8[0] = ' '; utf8[1] = 0; *cp_out = ' '; return ' ';
        }
        switch (vk) {
        case VK_UP:    return (alt && shift) ? K_ASSUP : alt ? K_UP : (ctrl ? K_CUP : (shift ? K_SUP : K_UP));
        case VK_DOWN:  return (alt && shift) ? K_ASSDOWN : alt ? K_DOWN : (ctrl ? K_CDOWN : (shift ? K_SDOWN : K_DOWN));
        case VK_LEFT:  return (alt && shift) ? K_ASSLEFT : alt ? META('b') : (ctrl ? (shift ? K_CSSLEFT : K_CLEFT) : (shift ? K_SLEFT : K_LEFT));
        case VK_RIGHT: return (alt && shift) ? K_ASSRIGHT : alt ? META('f') : (ctrl ? (shift ? K_CSSRIGHT : K_CRIGHT) : (shift ? K_SRIGHT : K_RIGHT));
        case VK_HOME:  return ctrl ? K_CHOME : (shift ? K_SHOME : K_HOME);
        case VK_END:   return ctrl ? K_CEND : (shift ? K_SEND : K_END);
        case VK_PRIOR: return ctrl ? K_CUP : (shift ? K_SPGUP : K_PGUP);
        case VK_NEXT:  return ctrl ? K_CDOWN : (shift ? K_SPGDN : K_PGDN);
        case VK_INSERT:return K_INS;
        case VK_DELETE:return ctrl ? K_CDEL : K_DEL;
        case VK_BACK:  utf8[0] = 127; utf8[1] = 0; return ctrl ? CTRL('h') : K_BACKSP;
        case VK_RETURN:return K_ENTER;
        case VK_TAB:   return shift ? META('z') : K_TAB;
        case VK_ESCAPE:return K_ESC;
        case VK_F1: return K_F1;  case VK_F2: return K_F2;  case VK_F3: return K_F3;
        case VK_F4: return K_F4;  case VK_F5: return K_F5;  case VK_F6: return K_F6;
        case VK_F7: return K_F7;  case VK_F8: return K_F8;  case VK_F9: return K_F9;
        case VK_F10: return K_F10; case VK_F11: return K_F11; case VK_F12: return K_F12;
        case VK_ADD: return ctrl ? CTRL('+') : K_UNKNOWN;
        case VK_SUBTRACT: return ctrl ? CTRL('-') : K_UNKNOWN;
        default: break;
        }
        if (uc >= 0x80 && !ctrl) {
            WCHAR w[2];
            int nw = 1;
            w[0] = uc;
            if (uc >= 0xD800 && uc <= 0xDBFF) {
                INPUT_RECORD nx;
                if (PeekConsoleInputW(t->hin, &nx, 1, &got) && got == 1 &&
                    nx.EventType == KEY_EVENT && nx.Event.KeyEvent.bKeyDown &&
                    nx.Event.KeyEvent.uChar.UnicodeChar >= 0xDC00) {
                    ReadConsoleInputW(t->hin, &nx, 1, &got);
                    w[nw++] = nx.Event.KeyEvent.uChar.UnicodeChar;
                }
            }
            char b[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, w, nw, b, 7, NULL, NULL);
            if (n > 0) {
                memcpy(utf8, b, n);
                utf8[n] = 0;
                int l;
                *cp_out = u8_decode(b, n, &l);
                return K_UNKNOWN;
            }
            return K_UNKNOWN;
        }
        if (uc >= 32 && uc <= 126) {
            if (ctrl) {
                int base = shift ? toupper(uc) : uc;
                int cc = base & 0x1F;
                return cc ? cc : 0;
            }
            if (alt) return META(uc);
            utf8[0] = (char)uc;
            utf8[1] = 0;
            *cp_out = uc;
            return uc;
        }
        if (uc >= 1 && uc <= 31) return uc;
        if (uc == 127) return K_BACKSP;
        return K_UNKNOWN;
    }
}

#else /* ====================== 类 Unix ====================== */

static volatile sig_atomic_t winch_flag;
static volatile sig_atomic_t quit_flag;

static void on_sig(int sig)
{
    if (sig == SIGWINCH) winch_flag = 1;
    else quit_flag = 1;
}

static const struct { const char *s; int k; } esctab[] = {
    { "[A", K_UP }, { "[B", K_DOWN }, { "[C", K_RIGHT }, { "[D", K_LEFT },
    { "[H", K_HOME }, { "[F", K_END }, { "[1~", K_HOME }, { "[4~", K_END },
    { "[2~", K_INS }, { "[3~", K_DEL }, { "[5~", K_PGUP }, { "[6~", K_PGDN },
    { "[7~", K_HOME }, { "[8~", K_END },
    { "[1;2A", K_SUP }, { "[1;2B", K_SDOWN }, { "[1;2C", K_SRIGHT }, { "[1;2D", K_SLEFT },
    { "[1;2H", K_SHOME }, { "[1;2F", K_SEND },
    { "[1;3A", K_SUP }, { "[1;3B", K_SDOWN }, { "[1;3C", K_SRIGHT }, { "[1;3D", K_SLEFT },
    { "[1;5A", K_CUP }, { "[1;5B", K_CDOWN }, { "[1;5C", K_CRIGHT }, { "[1;5D", K_CLEFT },
    { "[1;6C", K_CSSRIGHT }, { "[1;6D", K_CSSLEFT },
    { "[1;4C", K_ASSRIGHT }, { "[1;4D", K_ASSLEFT },
    { "[1;4A", K_ASSUP }, { "[1;4B", K_ASSDOWN },
    { "[1;5H", K_CHOME }, { "[1;5F", K_CEND }, { "[3;5~", K_CDEL },
    { "[5;5~", K_CUP }, { "[6;5~", K_CDOWN },
    { "OA", K_UP }, { "OB", K_DOWN }, { "OC", K_RIGHT }, { "OD", K_LEFT },
    { "OH", K_HOME }, { "OF", K_END },
    { "OP", K_F1 }, { "OQ", K_F2 }, { "OR", K_F3 }, { "OS", K_F4 },
    { "[11~", K_F1 }, { "[12~", K_F2 }, { "[13~", K_F3 }, { "[14~", K_F4 },
    { "[15~", K_F5 }, { "[17~", K_F6 }, { "[18~", K_F7 }, { "[19~", K_F8 },
    { "[20~", K_F9 }, { "[21~", K_F10 }, { "[23~", K_F11 }, { "[24~", K_F12 },
};

static int decode_seq(const char *seq, int n)
{
    for (size_t i = 0; i < sizeof esctab / sizeof esctab[0]; i++) {
        size_t l = strlen(esctab[i].s);
        if (l == (size_t)n && !memcmp(esctab[i].s, seq, n)) return esctab[i].k;
    }
    return K_UNKNOWN;
}

static int read_b(void)
{
    if (unix_pushb >= 0) { int v = unix_pushb; unix_pushb = -1; return v; }
    unsigned char c;
    ssize_t r;
    do { r = read(0, &c, 1); } while (r < 0 && errno == EINTR);
    return r <= 0 ? -1 : c;
}

static int unix_pushb = -1;

static int unix_peek(void)
{
    unsigned char c;
    ssize_t r;
    do {
        r = read(0, &c, 1);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;
    unix_pushb = c;
    return c;
}

/* \x1b[<b;x;yM -> K_MOUSE, 与 Windows 管道解析同一套语义 */
static int parse_sgr_mouse_unix(void)
{
    int b = 0, row = 0, col = 0, f;
    int x;
    while ((x = read_b()) >= '0' && x <= '9') b = b * 10 + (x - '0');
    while (x == ';') {
        int v = 0;
        while ((x = read_b()) >= '0' && x <= '9') v = v * 10 + (x - '0');
        if (!col) col = v; else row = v;
        if (x != ';') break;
    }
    f = x;
    ms_btn = b & 3;
    if (b & 64) ms_btn = (b & 1) ? 5 : 4;
    ms_press = (f == 'm') ? 0 : ((b & 32) ? 2 : 1);
    ms_ctrl = (b & 16) ? 1 : 0;                  /* SGR 里 0x10 = Ctrl 按住 */
    ms_row = row - 1;
    ms_col = col - 1;
    return (f == 'M' || f == 'm') ? K_MOUSE : K_UNKNOWN;
}

static int seq_pending(void)
{
    struct pollfd p;
    p.fd = 0;
    p.events = POLLIN;
    return poll(&p, 1, 40) > 0;
}

static void term_init(Term *t)
{
    t->tty = PEE_ISATTY(0) && PEE_ISATTY(1);
    t->full = 1;
    if (t->tty) {
        if (tcgetattr(0, &t->orig) == 0) {
            struct termios raw = t->orig;
            raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
            raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
            raw.c_oflag &= ~(OPOST);
            raw.c_cflag |= CS8;
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(0, TCSANOW, &raw) == 0) t->origok = 1;
        }
        signal(SIGWINCH, on_sig);
        signal(SIGTERM, on_sig);
        signal(SIGINT, on_sig);
        signal(SIGTSTP, SIG_DFL);
        term_write(t, "\x1b[?1049h", 8);
        term_write(t, "\x1b[?1000h\x1b[?1006h", 16);
    } else {
        t->rawpipe = 1;
        t->tty = 1;
    }
    term_size(t);
}

static void term_restore(Term *t)
{
    if (t->tty && !t->rawpipe) term_write(t, "\x1b[0m\x1b[?1049l", 12);
    if (t->origok) tcsetattr(0, TCSADRAIN, &t->orig);
}

static void term_size(Term *t)
{
    struct winsize ws;
    int w = 80, h = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { w = ws.ws_col; h = ws.ws_row; }
    else {
        const char *c = getenv("COLUMNS");
        const char *r = getenv("LINES");
        if (c && *c) w = atoi(c);
        if (r && *r) h = atoi(r);
    }
    if (w < 24) w = 24;
    if (h < 10) h = 10;
    if (w != t->w || h != t->h) { t->w = w; t->h = h; t->full = 1; }
}

static void term_write(Term *t, const char *s, size_t n)
{
    if (!t->tty) { fwrite(s, 1, n, stdout); fflush(stdout); return; }
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(1, s + off, n - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; }
        off += (size_t)w;
    }
}

static int term_read_key(Term *t, char *utf8, uint32_t *cp_out)
{
    utf8[0] = 0;
    *cp_out = 0;
    if (winch_flag) { winch_flag = 0; term_size(t); return K_RESIZE; }
    if (quit_flag) return K_NONE;
    int c = read_b();
    if (c < 0) return K_NONE;
    if (c == 27) {
        if (!seq_pending()) return K_ESC;
        int x = read_b();
        if (x < 0) return K_ESC;
        char seq[8];
        int n = 0;
        seq[n++] = (char)x;
        if (x == '[' && unix_peek() == '<') {
            read_b();
            return parse_sgr_mouse_unix();
        }
        if (x == '[' || x == 'O') {
            while (n < 7) {
                x = read_b();
                if (x < 0) break;
                seq[n++] = (char)x;
                if (x >= 0x40 && x <= 0x7E) break;
            }
            return decode_seq(seq, n);
        }
        return META(x);
    }
    if (c == '\n' || c == '\r') { utf8[0] = (char)K_ENTER; return K_ENTER; }
    if (c == '\t') { utf8[0] = (char)K_TAB; return K_TAB; }
    if (c == 8) { utf8[0] = 8; return CTRL('h'); }
    if (c == 127) { utf8[0] = 127; utf8[1] = 0; return K_BACKSP; }
    if (c < 32) return c;
    if (c < 0x80) { utf8[0] = (char)c; utf8[1] = 0; *cp_out = (uint32_t)c; return c; }
    char seq[4];
    int need = u8_seqlen((unsigned char)c);
    seq[0] = (char)c;
    for (int i = 1; i < need; i++) { int x = read_b(); seq[i] = x < 0 ? '?' : (char)x; }
    memcpy(utf8, seq, need);
    utf8[need] = 0;
    int l;
    *cp_out = u8_decode(seq, need, &l);
    return K_UNKNOWN;
}

/* 类 Unix: 没有稳定的通用剪贴板接口, 只用编辑器内部剪贴板 */
static void clipboard_put(const char *utf8, size_t n) { (void)utf8; (void)n; }
static char *clipboard_get(void) { return NULL; }

#endif

/* ====================== 布局常量 ====================== */
#define TEXT_TOP   1
static size_t text_rows(Term *t) { return (size_t)t->h - 4; }
static size_t status_row(Term *t) { return (size_t)t->h - 3; }   /* 0 基 */
/* 正文左边只留 1 列标记位(只读时是 >), 不显示行号 */
static size_t body_w(Term *t) { return (size_t)t->w - 1; }

/* 重建软换行映射: 每行按 body 宽度切成若干显示行 */
/* 某行内, 显示列 cell 落在第几个折行块(0 基) */
static size_t line_row_part(size_t li, size_t cell)
{
    size_t base = line_row0[li], top = line_row0[li + 1], k = 0;
    for (size_t r = base; r < top; r++) {
        if (map_col[r] <= cell) k = r - base;
        else break;
    }
    return k;
}

static void build_rowmap(void)
{
    size_t bw = body_w(&T);
    size_t li, r = 0, need = 0;
    if (bw < 4) bw = 4;
    line_row0 = xrealloc(line_row0, sizeof(size_t) * (D.n + 1));
    if (!atblanks) {
        for (li = 0; li < D.n; li++) {
            size_t parts = line_cells(&D.v[li], D.v[li].len) / bw + 1;
            line_row0[li] = need;
            need += parts;
        }
        line_row0[D.n] = need;
        if (need + 1 > map_cap) {
            map_cap = need + 64;
            map_line = xrealloc(map_line, sizeof(size_t) * map_cap);
            map_col = xrealloc(map_col, sizeof(size_t) * map_cap);
        }
        for (li = 0; li < D.n; li++) {
            size_t parts = line_row0[li + 1] - line_row0[li];
            for (size_t k = 0; k < parts; k++) {
                map_line[r] = li;
                map_col[r] = k * bw;
                r++;
            }
        }
        row_total = r;
        return;
    }
    /* atblanks: 贪心按显示宽度切, 能在空白处就断在这里 */
    for (li = 0; li < D.n; li++) {
        const Line *l = &D.v[li];
        size_t b = 0, cell = 0;
        line_row0[li] = r;
        for (;;) {
            if (r + 2 > map_cap) {
                map_cap = map_cap ? map_cap * 2 : 64;
                map_line = xrealloc(map_line, sizeof(size_t) * map_cap);
                map_col = xrealloc(map_col, sizeof(size_t) * map_cap);
            }
            map_line[r] = li;
            map_col[r] = cell;
            r++;
            {
                size_t target = cell + bw, i = b, c = cell;
                size_t lastsp = (size_t)-1, lastsp_c = 0;
                while (i < l->len && c < target) {
                    int len = 0;
                    uint32_t cp = u8_decode(l->b + i, l->len - i, &len);
                    size_t w;
                    if (len < 1) len = 1;
                    w = cellw(cp, c);
                    if (!w) w = 1;
                    if (c + w > target && i > b) break;
                    i += (size_t)len;
                    c += w;
                    if (cp == ' ' || cp == '\t') { lastsp = i; lastsp_c = c; }
                }
                if (i < l->len && lastsp != (size_t)-1 && lastsp > b) { i = lastsp; c = lastsp_c; }
                if (i == b) {                  /* 保证有进展 */
                    int len = (int)u8_seqlen((unsigned char)l->b[i]);
                    if (len < 1) len = 1;
                    if ((size_t)len > l->len - i) len = (int)(l->len - i);
                    i += (size_t)len;
                    c += cellw(u8_decode(l->b + b, l->len - b, &len), c);
                }
                b = i;
                cell = c;
            }
            if (b >= l->len) break;
        }
    }
    line_row0[D.n] = r;
    row_total = r;
}

/* 光标所在的显示行(全局) */
static int map_ready(void)
{
    return softwrap && line_row0 && row_total > 0;
}

static size_t cursor_row(void)
{
    size_t bw = body_w(&T);
    if (!map_ready()) return D.cur.line;      /* 折行地图还没建好时按行号算 */
    if (bw < 4) bw = 4;
    size_t cell = line_cells(&D.v[D.cur.line], D.cur.off);
    if (atblanks) return line_row0[D.cur.line] + line_row_part(D.cur.line, cell);
    return line_row0[D.cur.line] + cell / bw;
}


/* 选区: 返回 1 表示存在非空选区, 并把边界放到 *a *b */
static int selecting(Pos *a, Pos *b)
{
    if (!has_mark()) return 0;
    Pos m = D.mark, c = D.cur;
    if (pos_less(c, m)) { Pos x = m; m = c; c = x; }
    if (m.line == c.line && m.off == c.off) return 0;
    if (a) *a = m;
    if (b) *b = c;
    return 1;
}

/* ====================== 绘制 ====================== */
static void row_goto(Str *s, size_t row)
{
    str_putc(s, '\x1b');
    str_putc(s, '[');
    str_printf_num(s, row + 1);
    str_puts(s, ";1H");
}

static void emit_text(Str *s, const char *t, size_t n, size_t *used, size_t w)
{
    size_t i = 0;
    while (i < n && *used < w) {
        int len;
        uint32_t cp = u8_decode(t + i, n - i, &len);
        size_t cw = cellw(cp, *used);
        if (!cw) cw = 0;
        if (*used + cw > w) break;
        str_add(s, t + i, (size_t)len);
        *used += cw;
        i += len;
    }
}

static void draw_screen(Term *t)
{
    if (softwrap) build_rowmap();
    hl_pick(D.fname);
    /* 跨行块注释: 把"行首是否在注释里"推到视口底就够了 */
    cm_update(softwrap && row_total ? map_line[row_total - 1]
                                    : D.top + (status_row(t) - TEXT_TOP));
    Str *f = &t->frame;
    str_reset(f);
    size_t w = (size_t)t->w;
    size_t rows = (size_t)t->h;
    Pos sa, sb;
    int sel = selecting(&sa, &sb);

    for (size_t y = 0; y < rows; y++) {
        Str r;
        memset(&r, 0, sizeof r);
        size_t used = 0;
        if (y == 0) {                                     /* 标题行 */
            char left[256], right[64];
            snprintf(left, sizeof left, " %s " PEE_VERSION "  %s%s%s", PEE_NAME,
                     D.fname ? D.fname : "[新文件]",
                     D.enc == ENC_GBK ? " (GBK)" : "",
                     D.readonly ? " [只读]" : (D.modified ? " [已修改]" : ""));
            size_t cell = line_cells(&D.v[D.cur.line], D.cur.off);
            snprintf(right, sizeof right, "行 %zu/%zu 列 %zu %zu%%",
                     D.cur.line + 1, D.n, cell + 1,
                     D.n > 1 ? D.cur.line * 100 / (D.n - 1) : (size_t)100);
            str_puts(&r, uisgr[UC_TITLE]);
            str_putc(&r, ' ');
            str_puts(&r, left);
            used = 1 + str_cells(left);
            size_t rw2 = str_cells(right);
            if (used + rw2 + 1 <= w) {
                while (used + rw2 + 1 < w) { str_putc(&r, ' '); used++; }
                str_putc(&r, ' ');
                str_puts(&r, right);
                used += rw2 + 1;
            }
            str_puts(&r, "\x1b[m");
            while (used < w) { str_putc(&r, ' '); used++; }
        } else if (y >= status_row(t) + 1) {              /* 两行快捷键 */
            int i = (int)(y - status_row(t) - 1);
            static const char *bar[2][6] = {
                { "F1 帮助", "Ctrl+S 保存", "Ctrl+O 打开", "Ctrl+Q 退出", "Ctrl+A 全选", "Ctrl+T 跳行" },
                { "Ctrl+C 复制", "Ctrl+X 剪切", "Ctrl+V 粘贴", "Ctrl+F 查找", "Ctrl+R 替换", "Ctrl+D 删行" }
            };
            str_puts(&r, "\x1b[0m");
            str_puts(&r, uisgr[UC_BAR]);
            str_putc(&r, ' ');
            used = 1;
            for (int j = 0; j < 6; j++) {
                size_t cw = str_cells(bar[i][j]) + 3;
                if (used + cw > w) break;
                str_puts(&r, bar[i][j]);
                str_puts(&r, "   ");
                used += cw;
            }
            while (used < w) { str_putc(&r, ' '); used++; }
            str_puts(&r, "\x1b[m");
        } else if (y == status_row(t)) {                  /* 状态/消息行 */
            str_puts(&r, msg_err ? uisgr[UC_ERR] : uisgr[UC_STATUS]);
            if (msgbuf.len) emit_text(&r, msgbuf.b, msgbuf.len, &used, w);
            while (used < w) { str_putc(&r, ' '); used++; }
        } else if (help_mode) {                           /* 文本覆盖层(帮助 / 版本列表) */

            size_t li = (size_t)(help_ofs + (int)(y - TEXT_TOP));
            str_puts(&r, "\x1b[0m");
            if (pg_lines && li < (size_t)pg_n)
                emit_text(&r, pg_lines[li], strlen(pg_lines[li]), &used, w);
            while (used < w) { str_putc(&r, ' '); used++; }
        } else {                                          /* 正文 */
            size_t li, rstart, endcell = (size_t)-1;
            if (softwrap) {
                size_t rr = line_row0[D.top] + (y - TEXT_TOP);
                if (rr >= row_total) { li = (size_t)-1; rstart = 0; }
                else {
                    li = map_line[rr]; rstart = map_col[rr];
                    /* 这一块的终点: 下一块的起始列, 或行尾(atblanks 时块宽不一定等于屏宽) */
                    endcell = (rr + 1 < row_total && map_line[rr + 1] == li)
                              ? map_col[rr + 1] : (size_t)-1;
                }
            } else {
                li = D.top + (y - TEXT_TOP);
                rstart = D.sc;
            }
            str_puts(&r, "\x1b[0m");
            str_putc(&r, D.readonly ? '>' : ' ');
            used = 1;
            if (li >= D.n) {
                while (used < w) { str_putc(&r, ' '); used++; }
            } else {
                const Line *l = &D.v[li];
                size_t start = line_col_to_off(l, rstart);
                size_t cell = line_cells(l, start);
                size_t sel0 = 0, sel1 = (size_t)-1;
                if (rect_sel) {
                    size_t rl0, rl1, rc0, rc1;
                    if (rect_bounds(&rl0, &rl1, &rc0, &rc1) && li >= rl0 && li <= rl1) {
                        sel = 1; sel0 = rc0; sel1 = rc1;
                    } else if (rect_bounds(&rl0, &rl1, &rc0, &rc1)) sel = 0;
                } else if (sel) {
                    if (li == sa.line) sel0 = line_cells(l, sa.off);
                    if (li == sb.line) sel1 = line_cells(l, sb.off);
                }
                hl_line(l, li);
                size_t xcol[MC_MAX], xs0[MC_MAX], xs1[MC_MAX];
                const int xn = nmc ? mc_row_cells(li, xcol, xs0, xs1, MC_MAX) : 0;
                int inv = 0;
                int curcls = -1;
                size_t i = start;
                size_t trailcell = (size_t)-1;
                if (showblanks) {                  /* 行尾空白从哪一列开始 */
                    size_t k = l->len;
                    while (k > 0 && (l->b[k - 1] == ' ' || l->b[k - 1] == '\t')) k--;
                    trailcell = line_cells(l, k);
                }
                while (i < l->len && used < w && cell < endcell) {
                    int len;
                    uint32_t cp = u8_decode(l->b + i, l->len - i, &len);
                    size_t cw = cellw(cp, cell);
                    if (!cw) cw = 1;
                    if (used + cw > w) break;
                    int hi = (sel && li >= sa.line && li <= sb.line &&
                              cell >= sel0 && cell < sel1) || mc_cell_in(xn, xs0, xs1, cell);
                    int cls = (hilite_on && i < hl_len) ? hl_mask[i] : HL_NONE;
                    if (hi != inv || cls != curcls) {
                        /* SGR 是叠加的: 先打选区底色, 再叠语法色, 退出时补一次重置 */
                        inv = hi; curcls = cls;
                        if (hi) str_puts(&r, spot_on ? uisgr[UC_SPOT] : uisgr[UC_SEL]);
                        if (cls) str_puts(&r, hl_sgr(cls));
                        else if (!hi) str_puts(&r, "\x1b[0m");
                    }
                    if (showblanks && (cp == '\t' || (cp == ' ' && cell >= trailcell))) {
                        const char *mk = cp == '\t' ? ws_tab : ws_sp;
                        int ml = 0;
                        uint32_t mcp = u8_decode(mk, strlen(mk), &ml);
                        size_t mkw = u8_width(mcp) ? (size_t)u8_width(mcp) : 1;
                        str_puts(&r, mk);
                        for (size_t z = mkw; z < cw; z++) str_putc(&r, ' ');
                    } else
                        str_add(&r, l->b + i, (size_t)len);
                    cell += cw;
                    i += len;
                }
                if (inv) { str_puts(&r, "\x1b[m"); inv = 0; }
                size_t tcell = cell;
                while (used + (tcell - line_cells(l, start)) < w) {
                    /* 空格填充也按列推进 */
                    size_t cw = cellw(' ', tcell);
                    if (!cw) cw = 1;
                    if (used + cw > w) break;
                    str_putc(&r, ' ');
                    tcell += cw;
                    used += cw;
                }
                while (used < w) { str_putc(&r, ' '); used++; }
            }
        }
        row_goto(f, y);
        str_add(f, r.b ? r.b : "", r.len);
        free(r.b);
    }

    if (t->full || t->prev.len != f->len || memcmp(t->prev.b, f->b, f->len) != 0) {
        if (t->full) { str_reset(&t->prev); }
        term_write(t, "\x1b[H", 3);
        term_write(t, f->b ? f->b : "", f->len);
        str_reset(&t->prev);
        str_add(&t->prev, f->b ? f->b : "", f->len);
        t->full = 0;
    }

    /* 硬件光标 */
    if (!help_mode) {
        size_t cell = line_cells(&D.v[D.cur.line], D.cur.off);
        size_t ry = softwrap ? TEXT_TOP + (cursor_row() - line_row0[D.top])
                               : TEXT_TOP + (D.cur.line - D.top);
        size_t cx = 1 + (cell > D.sc ? cell - D.sc : 0);
        if (cx > w) cx = w;
        char csi[32];
        snprintf(csi, sizeof csi, "\x1b[%zu;%zuH", ry + 1, cx + 1);
        term_write(t, csi, strlen(csi));
    }
}

static void clamp_scroll(void)
{
    size_t bw = body_w(&T);
    size_t cell = line_cells(&D.v[D.cur.line], D.cur.off);
    size_t cwnd = 1;
    Line *l = &D.v[D.cur.line];
    if (D.cur.off < l->len) {
        int len;
        uint32_t cp = u8_decode(l->b + D.cur.off, l->len - D.cur.off, &len);
        cwnd = cellw(cp, cell);
        if (!cwnd) cwnd = 1;
    }
    if (cell + cwnd > D.sc + bw) D.sc = cell + cwnd > bw ? cell + cwnd - bw : 0;
    if (cell < D.sc) D.sc = cell;
    size_t rows = text_rows(&T);
    if (map_ready()) {
        D.sc = 0;
        if (cursor_row() < line_row0[D.top]) D.top = D.cur.line;
        while (D.top < D.n && cursor_row() >= line_row0[D.top] + rows) D.top++;
        return;
    }
    if (softwrap) D.sc = 0;
    if (D.cur.line < D.top) D.top = D.cur.line;
    if (rows && D.cur.line >= D.top + rows) D.top = D.cur.line - rows + 1;
}

/* ====================== 编辑 ====================== */
/* ---- 撤销/重做: 每步只记"改了哪一段"(行内字节增删 + 整行增删) ----
   改动坐标以记录时为基准, 逆向回放即还原; 同类连续操作(打字/退格)合并成一步。
   每步占用 = 实际改动的字节, 与文件大小无关, 所以大文件不会像整篇快照那样早早丢历史。
   撤销栈按缓冲各存一份, 切来切去不再清空历史。 */
#define UNDO_MAX_STEPS 2000
#define UNDO_MAX_BYTES (16u << 20)

typedef struct {
    int kind;                 /* 0 = 行内字节, 1 = 整行 */
    int dir;                  /* 1 = 当时插入, -1 = 当时删除 */
    size_t line;              /* 行号(记录当时的坐标系) */
    size_t off;               /* kind=0 的行内字节偏移 */
    char *b; size_t n;        /* kind=0 的字节 */
    Line *lv; size_t ln;      /* kind=1 的行副本(拥有所有权) */
} UCh;

struct Uop {
    UCh *c; size_t n, cap;
    size_t mem;
    Pos start, end;           /* 这一步开始前/结束后的光标 */
    int was_modified;
};

static Uop *cur_op;            /* 正在累积的那一步 */

static void op_reset(Uop *o)
{
    for (size_t i = 0; i < o->n; i++) {
        free(o->c[i].b);
        if (o->c[i].lv) {
            for (size_t j = 0; j < o->c[i].ln; j++) line_free(&o->c[i].lv[j]);
            free(o->c[i].lv);
        }
    }
    free(o->c);
    o->c = NULL;
    o->n = o->cap = 0;
    o->mem = 0;
}

/* 把累积的改动"移交"给栈(拥有 c 数组), 累加器清空但不释放 */
static void op_move(Uop *dst, Uop *src)
{
    *dst = *src;
    src->c = NULL;
    src->n = src->cap = src->mem = 0;
}

static void op_ensure(Uop *o)
{
    if (o->n + 1 > o->cap) {
        o->cap = o->cap ? o->cap * 2 : 16;
        o->c = xrealloc(o->c, sizeof(UCh) * o->cap);
    }
    memset(&o->c[o->n], 0, sizeof(UCh));
}

/* 记录一次行内字节变化; dir=1 表示"这些字节当时被插进来" */
static void rec_text(int dir, const Line *l, size_t off, const char *b, size_t n)
{
    size_t idx;
    if (!rec_op || !n || !b || l < D.v || l >= D.v + D.n) return;
    idx = (size_t)(l - D.v);
    op_ensure(rec_op);
    {
        UCh *c = &rec_op->c[rec_op->n];
        c->kind = 0;
        c->dir = dir;
        c->line = idx;
        c->off = off;
        c->n = n;
        c->b = xmalloc(n + 1);
        memcpy(c->b, b, n);
        c->b[n] = 0;
        rec_op->mem += n + sizeof(UCh);
        rec_op->n++;
    }
}

/* 记录整行增删: dir=1 时 i..i+k-1 是刚插进来的行, dir=-1 时是即将消失的行 */
static void rec_lines(int dir, size_t i, size_t k)
{
    if (!rec_op || !k || i + k > D.n) return;
    op_ensure(rec_op);
    {
        UCh *c = &rec_op->c[rec_op->n];
        c->kind = 1;
        c->dir = dir;
        c->line = i;
        c->ln = k;
        c->lv = xmalloc(sizeof(Line) * k);
        for (size_t j = 0; j < k; j++) {
            line_set(&c->lv[j], D.v[i + j].b, D.v[i + j].len);
            rec_op->mem += D.v[i + j].len + 1 + sizeof(Line);
        }
        rec_op->n++;
    }
}

static void remove_lines(size_t i, size_t k)
{
    if (i >= D.n || !k) return;
    if (i + k > D.n) k = D.n - i;
    rec_lines(-1, i, k);
    for (size_t j = 0; j < k; j++) line_free(&D.v[i + j]);
    memmove(&D.v[i], &D.v[i + k], (D.n - i - k) * sizeof(Line));
    D.n -= k;
    if (has_mark()) {
        if (D.mark.line >= i + k) D.mark.line -= k;
        else if (D.mark.line >= i) D.mark = NO_MARK();
    }
}

static void insert_lines_at(size_t i, const Line *src, size_t k)
{
    if (!k) return;
    if (i > D.n) i = D.n;
    doc_ensure(D.n + k);
    memmove(&D.v[i + k], &D.v[i], (D.n - i) * sizeof(Line));
    D.n += k;
    for (size_t j = 0; j < k; j++) line_set(&D.v[i + j], src[j].b, src[j].len);
    rec_lines(1, i, k);
    if (has_mark() && D.mark.line >= i) D.mark.line += k;
}

static void undo_push(Uop *o)
{
    if (D.un + 1 > D.ucap) {
        D.ucap = D.ucap ? D.ucap * 2 : 16;
        D.ustack = xrealloc(D.ustack, sizeof(Uop) * D.ucap);
    }
    op_move(&D.ustack[D.un], o);
    D.umem += D.ustack[D.un].mem;
    D.un++;
    while (D.un > UNDO_MAX_STEPS || D.umem > UNDO_MAX_BYTES) {
        D.umem -= D.ustack[0].mem;
        op_reset(&D.ustack[0]);
        memmove(&D.ustack[0], &D.ustack[1], (D.un - 1) * sizeof(Uop));
        D.un--;
    }
}

static void redo_push(Uop *o)
{
    if (D.rn + 1 > D.rcap) {
        D.rcap = D.rcap ? D.rcap * 2 : 16;
        D.rstack = xrealloc(D.rstack, sizeof(Uop) * D.rcap);
    }
    op_move(&D.rstack[D.rn], o);
    D.rmem += D.rstack[D.rn].mem;
    D.rn++;
    while (D.rn > UNDO_MAX_STEPS || D.rmem > UNDO_MAX_BYTES) {
        D.rmem -= D.rstack[0].mem;
        op_reset(&D.rstack[0]);
        memmove(&D.rstack[0], &D.rstack[1], (D.rn - 1) * sizeof(Uop));
        D.rn--;
    }
}

/* 结束当前步骤: 有改动就入撤销栈 */
static void seal_op(void)
{
    rec_op = NULL;
    if (!cur_op) return;
    if (cur_op->n) {
        cur_op->end = D.cur;
        undo_push(cur_op);
    }
    cur_op->n = 0;
    cur_op->mem = 0;
}

static void clear_redo(void)
{
    for (size_t i = 0; i < D.rn; i++) op_reset(&D.rstack[i]);
    D.rn = 0;
    D.rmem = 0;
}

/* 命令级修改前调用。kind 非 0 时, 与上一个同类命令合并成一步撤销 */
static void pre_edit(int kind)
{
    if (D.readonly) return;
    if (mc_depth && rec_op) return;              /* 多光标铺的一次编辑合成一步 */
    if (kind && D.batch_kind == kind && rec_op) return;
    seal_op();
    D.batch_kind = kind;
    if (!cur_op) {
        cur_op = xmalloc(sizeof(Uop));
        memset(cur_op, 0, sizeof *cur_op);
    }
    cur_op->n = 0;
    cur_op->mem = 0;
    cur_op->start = D.cur;
    cur_op->end = D.cur;
    cur_op->was_modified = D.modified;
    clear_redo();
    rec_op = cur_op;
}

/* 逆向回放 src 里那一步, 同时把逆向改动记进 newop */
static void op_replay(Uop *src, Uop *newop)
{
    op_reset(newop);
    newop->start = src->start;
    newop->end = src->end;
    newop->was_modified = D.modified;
    rec_op = newop;
    for (size_t z = src->n; z-- > 0; ) {
        UCh *c = &src->c[z];
        if (c->kind == 0) {
            if (c->line >= D.n) continue;
            {
                Line *l = &D.v[c->line];
                if (c->dir > 0) {
                    size_t off = c->off > l->len ? l->len : c->off;
                    size_t n = c->n;
                    if (off + n > l->len) n = l->len - off;
                    if (n) line_delete(l, off, n);
                } else if (c->off <= l->len) {
                    line_insert(l, c->off, c->b, c->n);
                }
            }
        } else if (c->dir > 0) {
            remove_lines(c->line, c->ln);
        } else {
            insert_lines_at(c->line, c->lv, c->ln);
        }
    }
    rec_op = NULL;
}

static void edit_reset(Term *t)
{
    D.batch_kind = 0;
    t->full = 1;
    D.mark = NO_MARK();
    clamp_cursor();
    clamp_scroll();
}

static void do_undo(Term *t)
{
    Uop src, inv;
    memset(&inv, 0, sizeof inv);
    seal_op();                             /* 在途那一步(比如刚打完的一串字)先结算, 否则撤不动 */
    if (!D.un) { set_msg("没有可撤销的了"); return; }
    src = D.ustack[--D.un];                /* 取出栈顶那一步(所有权移交 src) */
    D.umem -= src.mem;
    op_replay(&src, &inv);
    D.modified = src.was_modified;
    D.cur = src.start;
    op_reset(&src);
    redo_push(&inv);
    edit_reset(t);
    set_msg("已撤销");
}

static void do_redo(Term *t)
{
    Uop src, inv;
    memset(&inv, 0, sizeof inv);
    seal_op();
    if (!D.rn) { set_msg("没有可重做的了"); return; }
    src = D.rstack[--D.rn];
    D.rmem -= src.mem;
    op_replay(&src, &inv);
    D.modified = 1;
    D.cur = src.end;
    op_reset(&src);
    undo_push(&inv);
    edit_reset(t);
    set_msg("已重做");
}

/* 释放某个缓冲的撤销/重做记录 */
static void doc_free_undo(Doc *d)
{
    for (size_t i = 0; i < d->un; i++) op_reset(&d->ustack[i]);
    free(d->ustack);
    d->ustack = NULL;
    d->un = d->ucap = d->umem = 0;
    for (size_t i = 0; i < d->rn; i++) op_reset(&d->rstack[i]);
    free(d->rstack);
    d->rstack = NULL;
    d->rn = d->rcap = d->rmem = 0;
}

static void history_wipe(void)
{
    seal_op();
    rec_op = NULL;
    nmc = 0;                             /* 换内容/换缓冲时额外光标没有意义 */
    doc_free_undo(&D);
    D.batch_kind = 0;
}

static void remove_line(size_t i)
{
    remove_lines(i, 1);
}

static void insert_raw(const char *data, size_t n)
{
    Line *l = &D.v[D.cur.line];
    line_insert(l, D.cur.off, data, n);
    D.cur.off += n;
    D.modified = 1;
}

static void insert_text(const char *data, size_t n)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    pre_edit(1);
    Pos a, b;
    if (selecting(&a, &b)) { D.cur = a; D.mark = NO_MARK(); cut_range(a, b, 0); }
    insert_raw(data, n);
    D.mark = NO_MARK();
}

/* 行首空白字节数 */
static size_t leading_ws(const Line *l)
{
    size_t i = 0;
    while (i < l->len && (l->b[i] == ' ' || l->b[i] == '\t')) i++;
    return i;
}

static void do_insert_newline(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    Pos a, b;
    if (selecting(&a, &b)) { cut_range(a, b, 0); }
    Line *l = &D.v[D.cur.line];
    size_t lw = autoindent ? leading_ws(l) : 0;
    size_t cut = D.cur.off;
    char *ind = xmalloc(lw + 1);
    memcpy(ind, l->b, lw);
    size_t tail = l->len - D.cur.off;
    char *save = xmalloc(tail + 1);
    memcpy(save, l->b + D.cur.off, tail);
    line_delete(l, D.cur.off, tail);
    doc_ensure(D.n + 1);
    memmove(&D.v[D.cur.line + 2], &D.v[D.cur.line + 1],
            (D.n - D.cur.line - 1) * sizeof(Line));
    D.n++;
    line_set(&D.v[D.cur.line + 1], save, tail);
    rec_lines(1, D.cur.line + 1, 1);
    free(save);
    D.cur.line++;
    D.cur.off = 0;
    if (autoindent && lw) {
        Line *nl = &D.v[D.cur.line];
        size_t skip = 0;
        if (cut < lw) {          /* 光标原本落在缩进里 */
            while (skip < nl->len && (nl->b[skip] == ' ' || nl->b[skip] == '\t') && skip < lw - cut)
                skip++;
            line_delete(nl, 0, skip);
        }
        line_insert(nl, 0, ind, lw);
        D.cur.off = lw;
    }
    free(ind);
    D.modified = 1;
    D.mark = NO_MARK();
}

/* 取 [a,b) 的正文文本: 只读缓冲区字节, 行号栏绝不参与 */
static void insert_newline(void)
{
    pre_edit(0);
    do_insert_newline();
}

static void collect_range(Str *dst, Pos a, Pos b)
{
    if (pos_less(b, a)) { Pos x = a; a = b; b = x; }
    if (a.line == b.line) {
        if (b.off > a.off) str_add(dst, D.v[a.line].b + a.off, b.off - a.off);
        return;
    }
    str_add(dst, D.v[a.line].b + a.off, D.v[a.line].len - a.off);
    str_putc(dst, '\n');
    for (size_t i = a.line + 1; i < b.line; i++) {
        str_puts(dst, D.v[i].b);
        str_putc(dst, '\n');
    }
    if (b.off) str_add(dst, D.v[b.line].b, b.off);
}

/* 删除 [a,b), to_cut 时先把正文收进剪贴板 */
static void cut_range(Pos a, Pos b, int to_cut)
{
    if (pos_less(b, a)) { Pos x = a; a = b; b = x; }
    if (a.line == b.line && a.off == b.off) { D.mark = NO_MARK(); return; }
    if (to_cut) {
        str_reset(&cutbuf);
        collect_range(&cutbuf, a, b);
    }
    if (a.line == b.line) {
        line_delete(&D.v[a.line], a.off, b.off - a.off);
        D.cur = a;
        D.modified = 1;
        D.mark = NO_MARK();
        clamp_cursor();
        return;
    }
    line_delete(&D.v[a.line], a.off, D.v[a.line].len - a.off);
    while (a.line + 1 < b.line) { remove_line(a.line + 1); b.line--; }
    {   /* 此时原 b 行就在 a.line+1 */
        Line *src = &D.v[a.line + 1];
        line_insert(&D.v[a.line], D.v[a.line].len, src->b + b.off, src->len - b.off);
        remove_line(a.line + 1);
    }
    D.cur = a;
    D.modified = 1;
    D.mark = NO_MARK();
    clamp_cursor();
}

static void delete_selection(void)
{
    Pos a, b;
    if (selecting(&a, &b)) cut_range(a, b, 0);
}

static int last_was_cut;

/* ---------- 列块选择(Alt+Shift+方向) ---------- */
static void rect_start(void)
{
    if (!rect_sel) {
        D.mark = D.cur;
        rect_sel = 1;
    }
}

/* 列块边界: 行 [l0,l1], 显示列 [c0,c1) */
static int rect_bounds(size_t *l0, size_t *l1, size_t *c0, size_t *c1)
{
    if (!rect_sel || !has_mark()) return 0;
    size_t ca = line_cells(&D.v[D.mark.line], D.mark.off);
    size_t cb = line_cells(&D.v[D.cur.line], D.cur.off);
    if (ca == cb) return 0;
    *l0 = D.mark.line < D.cur.line ? D.mark.line : D.cur.line;
    *l1 = D.mark.line > D.cur.line ? D.mark.line : D.cur.line;
    *c0 = ca < cb ? ca : cb;
    *c1 = ca > cb ? ca : cb;
    if (*l0 >= D.n) *l0 = D.n - 1;
    if (*l1 >= D.n) *l1 = D.n - 1;
    return 1;
}

/* 取列块文本; wipe!=0 时同时删掉这些列 */
static void rect_text(Str *dst, int wipe)
{
    size_t l0, l1, c0, c1;
    if (!rect_bounds(&l0, &l1, &c0, &c1)) return;
    if (wipe) pre_edit(0);
    if (dst) str_reset(dst);
    for (size_t li = l0; li <= l1; li++) {
        Line *l = &D.v[li];
        size_t b = line_col_to_off(l, c0);
        size_t e = line_col_to_off(l, c1);
        if (e < b) e = b;
        if (dst) str_add(dst, l->b + b, e - b);
        if (wipe) line_delete(l, b, e - b);
        if (li < l1 && dst) str_putc(dst, '\n');
    }
    if (wipe) {
        D.cur = P(l0, line_col_to_off(&D.v[l0], c0));
        D.modified = 1;
        rect_sel = 0;
        D.mark = NO_MARK();
        clamp_cursor();
    }
}

/* 列块粘贴: 每行在目标列插入一行剪贴板内容, 短行用空格补齐 */
static void rect_paste(void)
{
    if (!cutbuf.len) { set_msg("剪贴板是空的"); return; }
    pre_edit(0);
    size_t col = line_cells(&D.v[D.cur.line], D.cur.off);
    size_t row = D.cur.line;
    size_t i = 0;
    for (;;) {
        size_t j = i;
        while (j < cutbuf.len && cutbuf.b[j] != '\n') j++;
        while (row >= D.n) insert_newline();
        Line *l = &D.v[row];
        size_t have = line_cells(l, l->len);
        while (have < col) { line_insert(l, l->len, " ", 1); have++; }
        size_t at = line_col_to_off(l, col);
        line_insert(l, at, cutbuf.b + i, j - i);
        if (row == D.cur.line) D.cur.off = at + (j - i);
        row++;
        if (j >= cutbuf.len) break;
        i = j + 1;
    }
    D.modified = 1;
    clamp_cursor();
    set_msg("列块粘贴完成");
}

/* Ctrl+C: 复制选区, 不删除 */
static void copy_selection(void)
{
    if (rect_sel) {
        str_reset(&cutbuf);
        rect_text(&cutbuf, 0);
        clipboard_put(cutbuf.b, cutbuf.len);
        set_msg("已复制列块 %zu 字节", cutbuf.len);
        return;
    }
    Pos a, b;
    if (!selecting(&a, &b)) { set_msg("没有选中内容"); return; }
    str_reset(&cutbuf);
    collect_range(&cutbuf, a, b);
    clipboard_put(cutbuf.b, cutbuf.len);
    last_was_cut = 0;
    rect_paste_mode = 0;
    set_msg("已复制 %zu 字节(不含行号)", cutbuf.len);
}

/* Ctrl+X: 有选区剪选区, 无选区剪当前行 */
static void cut_something(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    if (rect_sel) {
        rect_paste_mode = 1;
        str_reset(&cutbuf);
        rect_text(&cutbuf, 1);
        clipboard_put(cutbuf.b, cutbuf.len);
        set_msg("已剪切列块 %zu 字节", cutbuf.len);
        return;
    }
    pre_edit(0);
    Pos a, b;
    if (selecting(&a, &b)) {
        last_was_cut = 0;
        cut_range(a, b, 1);
        clipboard_put(cutbuf.b, cutbuf.len);
        return;
    }
    Line *l = &D.v[D.cur.line];
    if (cutfromcursor && D.cur.off < l->len) {      /* 只剪光标到行尾 */
        str_reset(&cutbuf);
        str_add(&cutbuf, l->b + D.cur.off, l->len - D.cur.off);
        pre_edit(0);
        line_delete(l, D.cur.off, l->len - D.cur.off);
        last_was_cut = 0;
        D.modified = 1;
        clipboard_put(cutbuf.b, cutbuf.len);
        set_msg("已剪切光标到行尾 %zu 字节", cutbuf.len);
        return;
    }
    if (last_was_cut) str_putc(&cutbuf, '\n');
    else str_reset(&cutbuf);
    last_was_cut = 1;
    str_puts(&cutbuf, l->b);
    if (D.n > 1) {
        remove_line(D.cur.line);
        clamp_cursor();
    } else {
        line_delete(l, 0, l->len);
    }
    clipboard_put(cutbuf.b, cutbuf.len);
    D.modified = 1;
    D.mark = NO_MARK();
}

/* Ctrl+D: 有选区删选区, 无选区删当前行(不进剪贴板) */
static void delete_something(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    if (rect_sel) { rect_text(NULL, 1); set_msg("已删除列块"); return; }
    pre_edit(0);
    Pos a, b;
    if (selecting(&a, &b)) { cut_range(a, b, 0); return; }
    last_was_cut = 0;
    if (D.n > 1) {
        remove_line(D.cur.line);
        clamp_cursor();
    } else {
        line_delete(&D.v[0], 0, D.v[0].len);
        D.cur.off = 0;
    }
    D.modified = 1;
    D.mark = NO_MARK();
}

/* Ctrl+V: 优先系统剪贴板, 取不到就用内部剪贴板 */
static void paste_cut(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    if (rect_paste_mode) { rect_paste(); return; }
    pre_edit(0);
    last_was_cut = 0;
    char *ext = clipboard_get();
    if (ext) {
        str_reset(&cutbuf);
        str_puts(&cutbuf, ext);
        free(ext);
    }
    if (!cutbuf.len) { set_msg("剪贴板是空的"); return; }
    delete_selection();
    /* 把剪贴板按换行切成若干段, 段之间插入换行 */
    size_t seg = 0;
    int first = 1;
    for (size_t i = 0; i <= cutbuf.len; i++) {
        if (i < cutbuf.len && cutbuf.b[i] != '\n') continue;
        if (!first) do_insert_newline();
        if (i > seg) insert_raw(cutbuf.b + seg, i - seg);
        first = 0;
        seg = i + 1;
    }
    D.modified = 1;
    D.mark = NO_MARK();
    clamp_cursor();
}

static void delete_char(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    pre_edit(2);
    if (has_mark() && selecting(NULL, NULL)) { delete_selection(); return; }
    Line *l = &D.v[D.cur.line];
    if (D.cur.off < l->len) {
        size_t e = D.cur.off + u8_seqlen((unsigned char)l->b[D.cur.off]);
        if (e > l->len) e = l->len;
        line_delete(l, D.cur.off, e - D.cur.off);
        D.modified = 1;
    } else if (D.cur.line + 1 < D.n) {
        Line *nx = &D.v[D.cur.line + 1];
        size_t old = l->len;
        line_insert(l, l->len, nx->b, nx->len);
        remove_line(D.cur.line + 1);
        D.cur.off = old;
        D.modified = 1;
    }
    D.mark = NO_MARK();
    clamp_cursor();
}

static void backspace(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    pre_edit(2);
    if (has_mark() && selecting(NULL, NULL)) { delete_selection(); return; }
    Line *l = &D.v[D.cur.line];
    if (D.cur.off > 0) {
        size_t i = D.cur.off - 1;
        while (i > 0 && ((unsigned char)l->b[i] & 0xC0) == 0x80) i--;
        line_delete(l, i, D.cur.off - i);
        D.cur.off = i;
        D.modified = 1;
    } else if (D.cur.line > 0) {
        size_t prev = D.cur.line - 1;
        size_t oldlen = D.v[prev].len;
        line_insert(&D.v[prev], oldlen, l->b, l->len);
        remove_line(D.cur.line);
        D.cur.line = prev;
        D.cur.off = oldlen;
        D.modified = 1;
    }
    D.mark = NO_MARK();
    clamp_cursor();
}

/* --- 缩进块: dir>0 加一级, dir<0 退一级; 作用于选区或当前行 --- */
static void block_range(size_t *a, size_t *b)
{
    Pos pa, pb;
    *a = D.cur.line;
    *b = D.cur.line;
    if (selecting(&pa, &pb)) {
        *a = pa.line;
        *b = pb.line;
        if (pb.off == 0 && *b > *a) (*b)--;      /* 选到行首不算那一行 */
    }
}

static void indent_block(int dir)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    size_t a, b;
    block_range(&a, &b);
    pre_edit(0);
    for (size_t i = a; i <= b; i++) {
        Line *l = &D.v[i];
        if (l->len == 0) continue;               /* 空行不缩进 */
        long delta;
        if (dir > 0) {
            line_insert(l, 0, "\t", 1);
            delta = 1;
        } else {
            size_t n = 0;
            if (l->b[0] == '\t') n = 1;
            else {
                while (n < l->len && n < tabstop && l->b[n] == ' ') n++;
                if (!n) continue;
            }
            line_delete(l, 0, n);
            delta = -(long)n;
        }
        if (D.cur.line == i) {
            long o = (long)D.cur.off + delta;
            D.cur.off = o < 0 ? 0 : (size_t)o;
        }
    }
    D.modified = 1;
    D.mark = NO_MARK();
    clamp_cursor();
    set_msg(dir > 0 ? "已缩进一级" : "已反缩进");
}

/* Alt+T: 行首空白在制表符与空格之间互换(nano 的 M-T) */
static void convert_indent(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    size_t a, b;
    block_range(&a, &b);
    pre_edit(0);
    for (size_t i = a; i <= b; i++) {
        Line *l = &D.v[i];
        size_t lw = leading_ws(l);
        if (!lw) continue;
        int has_tab = memchr(l->b, '\t', lw) != NULL;
        Str ws;
        memset(&ws, 0, sizeof ws);
        if (has_tab) {
            size_t col = 0;
            for (size_t k = 0; k < lw; k++)
                col += l->b[k] == '\t' ? tabstop - (col % tabstop) : 1;
            while (ws.len < col) str_putc(&ws, ' ');
        } else {
            size_t col = 0;
            while (col + tabstop <= lw) { str_putc(&ws, '\t'); col += tabstop; }
            while (col < lw) { str_putc(&ws, ' '); col++; }
        }
        line_delete(l, 0, lw);
        line_insert(l, 0, ws.b ? ws.b : "", ws.len);
        if (D.cur.line == i) D.cur.off = D.cur.off >= lw ? ws.len + D.cur.off - lw : ws.len;
        free(ws.b);
    }
    D.modified = 1;
    D.mark = NO_MARK();
    clamp_cursor();
    set_msg("已转换行首空白");
}

/* Tab 键: 补到下一个制表位(在制表位上用制表符, 其余用空格) */
static void tab_fill(void)
{
    size_t col = line_cells(&D.v[D.cur.line], D.cur.off);
    size_t need = tabstop - (col % tabstop);
    if (col % tabstop == 0) { insert_text("\t", 1); return; }
    char sp[32];
    if (need > sizeof sp - 1) need = sizeof sp - 1;
    memset(sp, ' ', need);
    insert_text(sp, need);
}

/* 覆盖模式: 光标处有字符时先顶掉整个码点 */
static void type_text(const char *data, size_t n)
{
    if (!overwrite || selecting(NULL, NULL)) { insert_text(data, n); return; }
    Line *l = &D.v[D.cur.line];
    if (D.cur.off < l->len) {
        int len;
        u8_decode(l->b + D.cur.off, l->len - D.cur.off, &len);
        pre_edit(1);
        line_delete(l, D.cur.off, (size_t)len);
    }
    insert_text(data, n);
}

/* 选区大小写转换: 0=大写 1=小写 2=词首大写; 没选区就转光标所在词 */
static void convert_case(int mode)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    Pos a, b;
    int had = selecting(&a, &b);
    if (!had) {
        Line *l = &D.v[D.cur.line];
        if (D.cur.off >= l->len || isspace((unsigned char)l->b[D.cur.off])) {
            set_msg("光标不在词上");
            return;
        }
        size_t st = D.cur.off, en = D.cur.off;
        while (st > 0 && !isspace((unsigned char)l->b[st - 1])) st--;
        while (en < l->len && !isspace((unsigned char)l->b[en])) en++;
        a = P(D.cur.line, st);
        b = P(D.cur.line, en);
    }
    pre_edit(0);
    for (size_t li = a.line; li <= b.line && li < D.n; li++) {
        Line *l = &D.v[li];
        size_t from = li == a.line ? a.off : 0;
        size_t to = li == b.line ? b.off : l->len;
        int word_start = 1;
        if (li == a.line && a.off > 0)
            word_start = isspace((unsigned char)l->b[a.off - 1]);
        for (size_t i = from; i < to && i < l->len; i++) {
            unsigned char c = (unsigned char)l->b[i];
            if (isspace(c)) { word_start = 1; continue; }
            unsigned char r = c;
            if (mode == 0) r = (unsigned char)toupper(c);
            else if (mode == 1) r = (unsigned char)tolower(c);
            else r = word_start ? (unsigned char)toupper(c) : (unsigned char)tolower(c);
            if (isalpha(c) && r != c) l->b[i] = (char)r;
            word_start = 0;
        }
    }
    D.modified = 1;
    D.mark = NO_MARK();
    clamp_cursor();
    set_msg(mode == 0 ? "已转大写" : (mode == 1 ? "已转小写" : "已词首大写"));
}

/* ====================== 移动 ====================== */
static void move_left(void)
{
    if (D.cur.off > 0) {
        size_t i = D.cur.off - 1;
        while (i > 0 && ((unsigned char)D.v[D.cur.line].b[i] & 0xC0) == 0x80) i--;
        D.cur.off = i;
    } else if (D.cur.line > 0) {
        D.cur.line--;
        D.cur.off = D.v[D.cur.line].len;
    }
}

static void move_right(void)
{
    Line *l = &D.v[D.cur.line];
    if (D.cur.off < l->len) {
        D.cur.off += (size_t)u8_seqlen((unsigned char)l->b[D.cur.off]);
        if (D.cur.off > l->len) D.cur.off = l->len;
    } else if (D.cur.line + 1 < D.n) {
        D.cur.line++;
        D.cur.off = 0;
    }
}

static void move_up(void)
{
    if (softwrap && row_total) {
        size_t bw = body_w(&T);
        if (bw < 4) bw = 4;
        size_t r = cursor_row();
        if (r == 0) { D.cur.line = 0; D.cur.off = 0; return; }
        size_t cell = line_cells(&D.v[D.cur.line], D.cur.off);
        size_t inrow = cell > map_col[r] ? cell - map_col[r] : 0;
        r--;
        D.cur.line = map_line[r];
        D.cur.off = line_col_to_off(&D.v[D.cur.line], map_col[r] + inrow);
        return;
    }
    if (D.cur.line == 0) { D.cur.off = 0; return; }
    D.cur.line--;
    D.cur.off = line_col_to_off(&D.v[D.cur.line], D.curow);
}

static void move_down(void)
{
    if (softwrap && row_total) {
        size_t bw = body_w(&T);
        if (bw < 4) bw = 4;
        size_t cell = line_cells(&D.v[D.cur.line], D.cur.off);
        size_t r = cursor_row();
        size_t inrow = cell > map_col[r] ? cell - map_col[r] : 0;
        if (r + 1 >= row_total) { D.cur.off = D.v[D.cur.line].len; return; }
        r++;
        D.cur.line = map_line[r];
        D.cur.off = line_col_to_off(&D.v[D.cur.line], map_col[r] + inrow);
        return;
    }
    if (D.cur.line + 1 >= D.n) { D.cur.off = D.v[D.cur.line].len; return; }
    D.cur.line++;
    D.cur.off = line_col_to_off(&D.v[D.cur.line], D.curow);
}

static int prev_is_space(void)
{
    Line *l = &D.v[D.cur.line];
    if (D.cur.off == 0) return 1;                       /* 行首左边是换行 */
    size_t i = D.cur.off - 1;
    while (i > 0 && ((unsigned char)l->b[i] & 0xC0) == 0x80) i--;
    return isspace((unsigned char)l->b[i]) ? 1 : 0;
}

static void word_back(void)
{
    if (D.cur.line == 0 && D.cur.off == 0) return;
    while (prev_is_space() && !(D.cur.line == 0 && D.cur.off == 0)) move_left();
    while (!prev_is_space() && !(D.cur.line == 0 && D.cur.off == 0)) move_left();
}

static void word_fwd(void)
{
    Line *l = &D.v[D.cur.line];
    /* 先跳出当前词 */
    while (D.cur.off < l->len && !isspace((unsigned char)l->b[D.cur.off])) move_right();
    /* 再跳过空白, 停在下一个词的开头 */
    for (;;) {
        l = &D.v[D.cur.line];
        if (D.cur.off >= l->len) {
            if (D.cur.line + 1 >= D.n) break;
            move_right();
            continue;
        }
        if (!isspace((unsigned char)l->b[D.cur.off])) break;
        move_right();
    }
}

static void go_home(void)
{
    Line *l = &D.v[D.cur.line];
    size_t i = 0;
    if (!smarthome) { D.cur.off = 0; return; }
    while (i < l->len && (l->b[i] == ' ' || l->b[i] == '\t')) i++;
    D.cur.off = (i == D.cur.off) ? 0 : i;
}

static void go_end(void) { D.cur.off = D.v[D.cur.line].len; }

/* move_cursor=1: ^Y/^V 移动光标; 0: ^R/^F 只滚动 */
static void page_move(int dir, int move_cursor)
{
    size_t rows = text_rows(&T);
    if (!rows) return;
    if (dir > 0) {
        if (move_cursor) {
            D.cur.line = D.cur.line + rows < D.n ? D.cur.line + rows : D.n - 1;
            D.top = D.cur.line + 1 > rows ? D.cur.line - rows + 1 : 0;
        } else {
            size_t lasttop = D.n > rows ? D.n - rows : 0;
            D.top = D.top + rows < lasttop ? D.top + rows : lasttop;
            if (D.cur.line < D.top) D.cur.line = D.top;
        }
    } else {
        if (move_cursor) {
            D.cur.line = D.cur.line > rows ? D.cur.line - rows : 0;
            D.top = D.cur.line;
        } else {
            D.top = D.top > rows ? D.top - rows : 0;
            if (D.cur.line > D.top + rows - 1) D.cur.line = D.top + rows - 1;
        }
    }
    if (D.cur.line >= D.n) D.cur.line = D.n - 1;
    D.cur.off = 0;
    D.curow = 0;
    clamp_cursor();
}

static void go_line(size_t n)
{
    D.cur.line = n;
    D.cur.off = 0;
    D.curow = 0;
    D.mark = NO_MARK();
    clamp_cursor();
    size_t rows = text_rows(&T);
    if (D.cur.line < D.top || D.cur.line >= D.top + rows)
        D.top = D.cur.line > rows / 2 ? D.cur.line - rows / 2 : 0;
}

#ifdef _WIN32
static long getpid_ish(void) { return (long)GetCurrentProcessId(); }
#else
static long getpid_ish(void) { return (long)getpid(); }
#endif

/* ====================== 路径补全 ====================== */
#define DIR_MAX 64                   /* 一次目录列举最多收多少条(文件名补全和快照列表共用) */
/* ---------- 快照(时间旅行存档)的尺寸约定 ---------- */
#define SNAP_SUB ".pee/snap"
#define SNAP_MAX DIR_MAX
#define SNAP_NL 512                     /* 快照名最长多少 */
#define SNAP_STL 15                     /* 时间戳位数: YYYYMMDD-HHMMSS */
#define SNAP_BIG (8u << 20)             /* 超过这么多字节就不留快照 */
static int prompt_tab;                 /* 当前提示框允许 Tab 补全文件名 */

#ifdef _WIN32
/* 列出 dir 下以 pfx 开头的条目 */
static int list_dir(const char *dir, const char *pfx, char out[DIR_MAX][512], int max)
{
    char pat[1024];
    if (!*dir) snprintf(pat, sizeof pat, "%s*", pfx);
    else snprintf(pat, sizeof pat, "%s/%s*", dir, pfx);
    for (char *q = pat; *q; q++) if (*q == '\\') *q = '/';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".")) continue;
        if (!strcmp(fd.cFileName, "..")) continue;
        snprintf(out[n], 512, "%s", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            strncat(out[n], "/", 511 - strlen(out[n]));
        n++;
    } while (n < max && FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int dir_is_rootlike(const char *d)
{
    return !*d || (d[1] == ':' && !d[2]);
}
#else
#include <dirent.h>
static int list_dir(const char *dir, const char *pfx, char out[DIR_MAX][512], int max)
{
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    size_t pl = strlen(pfx);
    while ((e = readdir(d)) && n < max) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (pl && strncmp(e->d_name, pfx, pl)) continue;
        snprintf(out[n], 512, "%s", e->d_name);
        if (e->d_type == DT_DIR) strncat(out[n], "/", 511 - strlen(out[n]));
        n++;
    }
    closedir(d);
    return n;
}

static int dir_is_rootlike(const char *d)
{
    return !*d;
}
#endif

/* 就地补全 buf 里最后一段路径; 返回提示语 */
static const char *complete_path(char *buf, size_t size, size_t *len, size_t *curp)
{
    static char names[DIR_MAX][512];
    char dir[512], pfx[512];
    size_t n = *len, slash = (size_t)-1;
    for (size_t i = 0; i < n; i++)
        if (buf[i] == '/' || buf[i] == '\\') slash = i;
    if (slash == (size_t)-1) { dir[0] = 0; snprintf(pfx, sizeof pfx, "%s", buf); }
    else {
        memcpy(dir, buf, slash);
        dir[slash] = 0;
        if (dir_is_rootlike(dir) && slash == 1 && buf[1] == ':') { dir[2] = '/'; dir[3] = 0; }
        snprintf(pfx, sizeof pfx, "%s", buf + slash + 1);
    }
    int cnt = list_dir(dir, pfx, names, DIR_MAX);
    if (!cnt) return "没有匹配的文件";
    size_t common = strlen(names[0]);
    for (int i = 1; i < cnt; i++) {
        size_t k = 0;
        while (k < common && names[i][k] == names[0][k]) k++;
        common = k;
    }
    if (cnt == 1) {
        size_t base = (slash == (size_t)-1) ? 0 : slash + 1;
        size_t want = base + strlen(names[0]);
        if (want + 1 >= size) return "名字太长";
        memcpy(buf + base, names[0], strlen(names[0]));
        *len = want;
        buf[*len] = 0;
        *curp = *len;
        return NULL;
    }
    size_t base = (slash == (size_t)-1) ? 0 : slash + 1;
    size_t have = strlen(pfx);
    if (common <= have) return "多个候选, 再打几个字母";
    if (base + common + 1 >= size) return "名字太长";
    memcpy(buf + base, names[0], common);
    *len = base + common;
    buf[*len] = 0;
    *curp = *len;
    return NULL;
}

/* ====================== 输入历史 ====================== */
enum { HIST_SEARCH = 0, HIST_REPLACE, HIST_FILE, HIST_CMD, NHIST };
#define HIST_MAX 50
static char *hist[NHIST][HIST_MAX];
static int hist_n[NHIST];
static int prompt_hist = -1;

static const char *hist_pick(int which, int idx)
{
    if (which < 0 || which >= NHIST) return "";
    if (idx < 0 || idx >= hist_n[which]) return "";
    return hist[which][idx];
}

static void hist_add(int which, const char *v)
{
    if (!*v || which < 0 || which >= NHIST) return;
    for (int i = 0; i < hist_n[which]; i++)
        if (!strcmp(hist[which][i], v)) return;
    if (hist_n[which] < HIST_MAX) {
        memmove(&hist[which][1], &hist[which][0], (size_t)hist_n[which] * sizeof(char *));
        hist[which][0] = xstrdup(v);
        hist_n[which]++;
    } else {
        free(hist[which][HIST_MAX - 1]);
        memmove(&hist[which][1], &hist[which][0], (HIST_MAX - 1) * sizeof(char *));
        hist[which][0] = xstrdup(v);
    }
}

static const char *hist_path(void)
{
    static char buf[1024];
    const char *e = getenv("HOME");
    if (e) { snprintf(buf, sizeof buf, "%s/.pee_history", e); return buf; }
    e = getenv("APPDATA");
    if (e) { snprintf(buf, sizeof buf, "%s\\pee_history", e); return buf; }
    return ".pee_history";
}

static void hist_load(void)
{
    size_t len;
    int err;
    char *data = read_file(hist_path(), &len, &err);
    if (!data) return;
    int which = HIST_SEARCH;
    char *ln = data;
    while (ln) {
        char *nl = strchr(ln, '\n');
        if (nl) *nl++ = 0;
        if (!strcmp(ln, "#search")) which = HIST_SEARCH;
        else if (!strcmp(ln, "#replace")) which = HIST_REPLACE;
        else if (!strcmp(ln, "#file")) which = HIST_FILE;
        else if (!strcmp(ln, "#command")) which = HIST_CMD;
        else if (!strcmp(ln, "#position")) which = NHIST;
        else if (*ln && ln[0] != '#') {
            if (which == NHIST) {
                char *t1 = strchr(ln, '\t');
                char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
                if (t1 && t2) {
                    *t1 = 0;
                    *t2 = 0;
                    pos_remember(ln, atol(t1 + 1), atol(t2 + 1));
                }
            } else {
                /* 文件里已经是"新→旧", 直接按顺序入表;
                   用 hist_add 会把顺序倒过来, ↑ 就变成召回最旧的一条了 */
                int dup = 0;
                for (int i = 0; i < hist_n[which]; i++)
                    if (!strcmp(hist[which][i], ln)) { dup = 1; break; }
                if (!dup && hist_n[which] < HIST_MAX)
                    hist[which][hist_n[which]++] = xstrdup(ln);
            }
        }
        ln = nl;
    }
    free(data);
}

/* 每个文件上次的光标位置(nano 的 --positionlog) */
#define POS_MAX 30
static char *pos_name[POS_MAX];
static long pos_ln[POS_MAX], pos_col[POS_MAX];
static int pos_n;

#ifdef _WIN32
/* 位置表按绝对路径记, 避免同名相对路径串位 */
static const char *abs_path(const char *name)
{
    static char buf[2048];
    DWORD n = GetFullPathNameA(name, (DWORD)sizeof buf, buf, NULL);
    if (n == 0 || n >= sizeof buf) snprintf(buf, sizeof buf, "%s", name);
    return buf;
}
#else
static const char *abs_path(const char *name)
{
    static char buf[2048];
    if (!realpath(name, buf)) snprintf(buf, sizeof buf, "%s", name);
    return buf;
}
#endif

static void pos_remember(const char *name, long line, long col)
{
    for (int i = 0; i < pos_n; i++) {
        if (!strcmp(pos_name[i], abs_path(name))) { pos_ln[i] = line; pos_col[i] = col; return; }
    }
    if (pos_n < POS_MAX) {
        pos_name[pos_n] = xstrdup(abs_path(name));
        pos_ln[pos_n] = line;
        pos_col[pos_n] = col;
        pos_n++;
    }
}

static int pos_recall(const char *name, long *line, long *col)
{
    for (int i = 0; i < pos_n; i++)
        if (!strcmp(pos_name[i], abs_path(name))) { *line = pos_ln[i]; *col = pos_col[i]; return 1; }
    return 0;
}

static void hist_save(void)
{
    FILE *f = fopen(hist_path(), "wb");
    if (!f) return;
    static const char *tags[NHIST] = { "#search", "#replace", "#file", "#command" };
    for (int w = 0; w < NHIST; w++) {
        fputs(tags[w], f);
        fputc('\n', f);
        for (int i = 0; i < hist_n[w]; i++) { fputs(hist[w][i], f); fputc('\n', f); }
    }
    fputs("#position\n", f);
    for (int i = 0; i < pos_n; i++)
        fprintf(f, "%s\t%ld\t%ld\n", pos_name[i], pos_ln[i], pos_col[i]);
    fclose(f);
}

/* ====================== 迷你正则引擎 ====================== */
/* 自带实现, 不依赖第三方: 支持 . * + ? {m,n} | () [] ^ $ \d \w \s \b 与 \ 转义;
 * 不支持反向引用。语法解析成 AST, 匹配用带回溯的解释器(有步数上限防爆栈)。 */
enum {
    RN_EMPTY, RN_CHAR, RN_ANY, RN_CLASS, RN_BOL, RN_EOL, RN_WB, RN_NWB,
    RN_SEQ, RN_ALT, RN_REP, RN_GROUP, RN_SAVE
};

typedef struct RNode {
    int type;
    uint32_t cp;                       /* RN_CHAR */
    int neg;                           /* RN_CLASS 取反 */
    struct { uint32_t lo, hi; } rg[12];
    int nr;
    int min, max, greedy;              /* RN_REP */
    struct RNode *a, *b;
} RNode;

typedef struct {
    RNode *pool;
    int np, cap;
    int err;
    RNode *root;
} Prog;

#define PROG_NODES 4096
#define RE_STEPS 200000

static void re_free(Prog *g)
{
    if (!g) return;
    free(g->pool);
    free(g);
}

static RNode *rn_new(Prog *g)
{
    if (g->np >= g->cap) { g->err = 1; return &g->pool[g->cap - 1]; }
    RNode *n = &g->pool[g->np++];
    memset(n, 0, sizeof *n);
    n->max = 1;
    n->greedy = 1;
    return n;
}

typedef struct { const char *s; size_t i; Prog *g; int ngrp; } RP;

static int re_peek(RP *r) { return (unsigned char)r->s[r->i]; }

static uint32_t re_next(RP *r)
{
    int l;
    uint32_t cp = u8_decode(r->s + r->i, strlen(r->s + r->i), &l);
    r->i += (size_t)l;
    return cp;
}

static void re_add_range(RNode *n, uint32_t lo, uint32_t hi)
{
    if (n->nr >= 12) return;
    n->rg[n->nr].lo = lo;
    n->rg[n->nr].hi = hi;
    n->nr++;
}

/* \d \D \w \W \s \S -> 填进 cls 的区间表; 命中返回 1 */
static int re_shortcut(RNode *cls, int e)
{
    switch (e) {
    case 'd': re_add_range(cls, '0', '9'); return 1;
    case 'D': re_add_range(cls, 1, '0' - 1); re_add_range(cls, '9' + 1, 0x10FFFF); return 1;
    case 'w': re_add_range(cls, '0', '9'); re_add_range(cls, 'A', 'Z');
              re_add_range(cls, '_', '_'); re_add_range(cls, 'a', 'z'); return 1;
    case 'W': re_add_range(cls, 1, 47); re_add_range(cls, 58, 64);
              re_add_range(cls, 91, 94); re_add_range(cls, 96, 96);
              re_add_range(cls, 123, 0x10FFFF); return 1;
    case 's': re_add_range(cls, 9, 13); re_add_range(cls, ' ', ' '); return 1;
    case 'S': re_add_range(cls, 1, 8); re_add_range(cls, 14, 31);
              re_add_range(cls, 33, 0x10FFFF); return 1;
    default: return 0;
    }
}

static RNode *re_alt(RP *r);

static void re_class(RP *r, RNode *n)
{
    Prog *g = r->g;
    if (re_peek(r) == '^') { r->i++; n->neg = 1; }
    if (re_peek(r) == ']') r->i++;                    /* 首位 ] 视为字面量 */
    while (r->s[r->i] && r->s[r->i] != ']') {
        uint32_t lo;
        if (r->s[r->i] == '\\') {
            r->i++;
            int e = re_peek(r);
            if (!e) { g->err = 1; return; }
            r->i++;
            RNode tmp;
            memset(&tmp, 0, sizeof tmp);
            if (re_shortcut(&tmp, e)) {
                for (int k = 0; k < tmp.nr; k++) re_add_range(n, tmp.rg[k].lo, tmp.rg[k].hi);
                continue;
            }
            lo = (uint32_t)e;
        } else lo = re_next(r);
        if (r->s[r->i] == '-' && r->s[r->i + 1] && r->s[r->i + 1] != ']') {
            r->i++;
            uint32_t hi;
            if (r->s[r->i] == '\\') {
                r->i++;
                int e = re_peek(r);
                if (!e) { g->err = 1; return; }
                r->i++;
                RNode tmp;
                memset(&tmp, 0, sizeof tmp);
                if (re_shortcut(&tmp, e)) {
                    for (int k = 0; k < tmp.nr; k++) re_add_range(n, tmp.rg[k].lo, tmp.rg[k].hi);
                    re_add_range(n, lo, lo);
                    continue;
                }
                hi = (uint32_t)e;
            } else hi = re_next(r);
            if (hi < lo) { g->err = 1; return; }
            re_add_range(n, lo, hi);
        } else re_add_range(n, lo, lo);
    }
    if (r->s[r->i] != ']') { g->err = 1; return; }
    r->i++;
}

static RNode *re_atom(RP *r)
{
    Prog *g = r->g;
    int c = re_peek(r);
    if (!c) { g->err = 1; return NULL; }
    switch (c) {
    case '(': {
        r->i++;
        if (r->ngrp >= 9) { g->err = 1; return NULL; }
        int k = ++r->ngrp;
        RNode *content = re_alt(r);
        if (re_peek(r) != ')') { g->err = 1; return NULL; }
        r->i++;
        RNode *so = rn_new(g), *sc = rn_new(g), *t1 = rn_new(g), *t2 = rn_new(g);
        so->type = RN_SAVE; so->min = 2 * k;
        sc->type = RN_SAVE; sc->min = 2 * k + 1;
        t1->type = RN_SEQ; t1->a = content; t1->b = sc;
        t2->type = RN_SEQ; t2->a = so; t2->b = t1;
        RNode *n = rn_new(g);
        n->type = RN_GROUP;
        n->a = t2;
        return n;
    }
    case '[': {
        r->i++;
        RNode *n = rn_new(g);
        n->type = RN_CLASS;
        re_class(r, n);
        if (g->err) return NULL;
        return n;
    }
    case '.': r->i++; { RNode *n = rn_new(g); n->type = RN_ANY; return n; }
    case '^': r->i++; { RNode *n = rn_new(g); n->type = RN_BOL; return n; }
    case '$': r->i++; { RNode *n = rn_new(g); n->type = RN_EOL; return n; }
    case '\\': {
        r->i++;
        int e = re_peek(r);
        if (!e) { g->err = 1; return NULL; }
        r->i++;
        if (e == 'b' || e == 'B') {
            RNode *n = rn_new(g);
            n->type = e == 'b' ? RN_WB : RN_NWB;
            return n;
        }
        RNode cls;
        memset(&cls, 0, sizeof cls);
        if (re_shortcut(&cls, e)) {
            RNode *n = rn_new(g);
            n->type = RN_CLASS;
            for (int k = 0; k < cls.nr; k++) re_add_range(n, cls.rg[k].lo, cls.rg[k].hi);
            return n;
        }
        RNode *n = rn_new(g);
        n->type = RN_CHAR;
        n->cp = e == 'n' ? '\n' : e == 't' ? '\t' : (uint32_t)e;
        return n;
    }
    default: {
        RNode *n = rn_new(g);
        n->type = RN_CHAR;
        n->cp = re_next(r);
        return n;
    }
    }
}

static RNode *re_rep(RP *r)
{
    Prog *g = r->g;
    RNode *a = re_atom(r);
    if (!a) return NULL;
    int min = 1, max = 1, greedy = 1, have = 0;
    int c = re_peek(r);
    if (c == '*') { r->i++; min = 0; max = 1 << 28; have = 1; }
    else if (c == '+') { r->i++; min = 1; max = 1 << 28; have = 1; }
    else if (c == '?') { r->i++; min = 0; max = 1; have = 1; }
    else if (c == '{') {
        const char *q = r->s + r->i + 1;
        char *endp;
        long lo = strtol(q, &endp, 10);
        if (endp == q) { g->err = 1; return NULL; }
        long hi = lo;
        if (*endp == ',') {
            endp++;
            if (*endp == '}') hi = 1 << 28;
            else { hi = strtol(endp, &endp, 10); if (hi < lo) { g->err = 1; return NULL; } }
        }
        if (*endp != '}') { g->err = 1; return NULL; }
        r->i = (size_t)(endp - r->s) + 1;
        min = (int)lo;
        max = (int)hi;
        have = 1;
    }
    if (!have) return a;
    if (re_peek(r) == '?') { r->i++; greedy = 0; }        /* 惰性量词 a*? */
    RNode *n = rn_new(g);
    n->type = RN_REP;
    n->a = a;
    n->min = min;
    n->max = max;
    n->greedy = greedy;
    return n;
}

static RNode *re_seq(RP *r)
{
    RNode *list[64];
    int n = 0;
    while (re_peek(r) && re_peek(r) != '|' && re_peek(r) != ')') {
        RNode *x = re_rep(r);
        if (!x) return NULL;
        if (n >= 64) { r->g->err = 1; return NULL; }
        list[n++] = x;
    }
    if (!n) {
        RNode *e = rn_new(r->g);
        e->type = RN_EMPTY;
        return e;
    }
    RNode *acc = list[n - 1];
    for (int i = n - 2; i >= 0; i--) {
        RNode *s2 = rn_new(r->g);
        s2->type = RN_SEQ;
        s2->a = list[i];
        s2->b = acc;
        acc = s2;
    }
    return acc;
}

static RNode *re_alt(RP *r)
{
    RNode *a = re_seq(r);
    if (!a) return NULL;
    while (re_peek(r) == '|') {
        r->i++;
        RNode *b = re_seq(r);
        if (!b) return NULL;
        RNode *n = rn_new(r->g);
        n->type = RN_ALT;
        n->a = a;
        n->b = b;
        a = n;
    }
    return a;
}

static Prog *re_compile(const char *pat)
{
    Prog *g = xmalloc(sizeof(Prog));
    memset(g, 0, sizeof *g);
    g->cap = PROG_NODES;
    g->pool = xmalloc(sizeof(RNode) * (size_t)g->cap);
    RP r;
    r.s = pat;
    r.i = 0;
    r.g = g;
    r.ngrp = 0;
    g->root = re_alt(&r);
    if (g->err || !g->root || r.s[r.i]) {
        re_free(g);
        return NULL;
    }
    return g;
}

typedef struct Cc { const RNode *n; int cnt; const struct Cc *next; } Cc;

static long re_steps;
static size_t re_cap[20];   /* 捕获组位置: [2k]=起 [2k+1]=止, 0 是整段 */

static int re_is_word_at(const char *s, size_t len, size_t sp, int back)
{
    if (!back) {
        if (sp >= len) return 0;
        int l;
        uint32_t cp = u8_decode(s + sp, len - sp, &l);
        return (cp == '_' || (cp < 128 && isalnum((int)cp))) ? 1 : 0;
    }
    if (sp == 0) return 0;
    size_t j = sp - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
    int l;
    uint32_t cp = u8_decode(s + j, sp - j, &l);
    return (cp == '_' || (cp < 128 && isalnum((int)cp))) ? 1 : 0;
}

static int re_class_hit(const RNode *n, uint32_t cp)
{
    int hit = 0;
    for (int i = 0; i < n->nr; i++)
        if (cp >= n->rg[i].lo && cp <= n->rg[i].hi) { hit = 1; break; }
    return n->neg ? !hit : hit;
}

static int re_run(const Cc *c, const char *s, size_t len, size_t sp, size_t *end)
{
    if (++re_steps > RE_STEPS) return 0;
    if (!c || !c->n) { *end = sp; return 1; }
    const RNode *n = c->n;
    const Cc *rest = c->next;
    switch (n->type) {
    case RN_EMPTY: return re_run(rest, s, len, sp, end);
    case RN_SEQ: {
        Cc t2 = { n->b, 0, rest };
        Cc t1 = { n->a, 0, &t2 };
        return re_run(&t1, s, len, sp, end);
    }
    case RN_ALT: {
        Cc t1 = { n->a, 0, rest };
        if (re_run(&t1, s, len, sp, end)) return 1;
        Cc t2 = { n->b, 0, rest };
        return re_run(&t2, s, len, sp, end);
    }
    case RN_GROUP:
    case RN_SAVE: {
        if (n->type == RN_SAVE) {
            size_t old = re_cap[n->min];
            re_cap[n->min] = sp;
            int r = re_run(rest, s, len, sp, end);
            if (!r) re_cap[n->min] = old;      /* 回溯才清, 成功要留住捕获位置 */
            return r;
        }
        Cc t1 = { n->a, 0, rest };
        return re_run(&t1, s, len, sp, end);
    }
    case RN_REP: {
        int cnt = c->cnt;
        if (n->greedy) {
            if (cnt < n->max) {
                Cc again = { n, cnt + 1, rest };
                Cc one = { n->a, 0, &again };
                if (re_run(&one, s, len, sp, end)) return 1;
            }
            if (cnt >= n->min) return re_run(rest, s, len, sp, end);
            return 0;
        }
        if (cnt >= n->min && re_run(rest, s, len, sp, end)) return 1;
        if (cnt < n->max) {
            Cc again = { n, cnt + 1, rest };
            Cc one = { n->a, 0, &again };
            return re_run(&one, s, len, sp, end);
        }
        return 0;
    }
    case RN_BOL: return sp == 0 ? re_run(rest, s, len, sp, end) : 0;
    case RN_EOL: return sp == len ? re_run(rest, s, len, sp, end) : 0;
    case RN_WB:
    case RN_NWB: {
        int w1 = re_is_word_at(s, len, sp, 1);
        int w2 = re_is_word_at(s, len, sp, 0);
        int isb = (w1 != w2);
        if ((n->type == RN_WB) != isb) return 0;
        return re_run(rest, s, len, sp, end);
    }
    case RN_ANY: {
        if (sp >= len) return 0;
        int l;
        uint32_t cp = u8_decode(s + sp, len - sp, &l);
        if (cp == '\n') return 0;
        return re_run(rest, s, len, sp + (size_t)l, end);
    }
    case RN_CLASS: {
        if (sp >= len) return 0;
        int l;
        uint32_t cp = u8_decode(s + sp, len - sp, &l);
        if (!re_class_hit(n, cp)) return 0;
        return re_run(rest, s, len, sp + (size_t)l, end);
    }
    case RN_CHAR: {
        if (sp >= len) return 0;
        int l;
        uint32_t cp = u8_decode(s + sp, len - sp, &l);
        if (cp != n->cp) return 0;
        return re_run(rest, s, len, sp + (size_t)l, end);
    }
    }
    return 0;
}

/* 把替换串里的 \1..\9 换成刚匹配到的分组文字 */
static void expand_repl(const char *rp, Str *out, const char *line)
{
    for (; *rp; rp++) {
        if (*rp == '\\' && rp[1] >= '1' && rp[1] <= '9') {
            int k = rp[1] - '0';
            size_t a = re_cap[2 * k], b = re_cap[2 * k + 1];
            rp++;
            if (b > a)
                str_add(out, line + a, b - a);
            continue;
        }
        str_putc(out, *rp);
    }
}

/* 在 [from, len) 内找第一个匹配, 输出字节区间 [b,e) */
static int re_exec(Prog *g, const char *s, size_t len, size_t from, size_t *b, size_t *e)
{
    if (!g) return 0;
    Cc startc = { g->root, 0, NULL };
    for (size_t sp = from; sp <= len; ) {
        re_steps = 0;
        memset(re_cap, 0, sizeof re_cap);
        size_t en = sp;
        if (re_run(&startc, s, len, sp, &en)) {
            *b = sp;
            *e = en;
            re_cap[0] = sp;
            re_cap[1] = en;
            return 1;
        }
        if (sp >= len) break;
        int l = u8_seqlen((unsigned char)s[sp]);
        if ((size_t)l == 0 || sp + (size_t)l > len) break;
        sp += (size_t)l;
    }
    return 0;
}

/* ====================== 语法高亮与配色 ====================== */
/* 规则可以从 pee.cfg 里写(见 load_config 的 syntax/color/comment 段), 没配就用下面内置的默认语法。
   一条规则 = 一段 SGR + 一个自带正则; 掩码存"规则号+1"(HL_BLOCK 表示块注释延续),
   先声明的规则先占住字节, 所以字符串/注释要写在关键字前面。 */
static int pee_caseq(const char *a, const char *b, size_t n);
static int pee_casew(const char *a, const char *b);

typedef struct {
    char sgr[32];
    const char *pat;
    Prog *g;
    Prog *gi;                 /* icolor: 用小写模式配小写化后的行 */
    int icase;
} HlRule;

typedef struct {
    const char *name;
    const char *filepat;              /* 匹配文件名的正则; NULL = 适用所有文件 */
    Prog *fileg;
    const char *cmt_open, *cmt_close; /* 块注释对, 用于跨行注释状态 */
    char cmt_sgr[32];
    HlRule rule[32];
    int nrule;
} Syntax;

#define SYN_MAX 32
static Syntax syn[SYN_MAX];
static int nsyn;
static Syntax *cur_syn;
static char hl_for[1024];

static const struct { const char *w; int code; } sgr_names[] = {
    {"black", 30}, {"red", 31}, {"green", 32}, {"yellow", 33}, {"blue", 34},
    {"magenta", 35}, {"cyan", 36}, {"white", 37}, {"default", 39},
    {"brightblack", 90}, {"brightred", 91}, {"brightgreen", 92}, {"brightyellow", 93},
    {"brightblue", 94}, {"brightmagenta", 95}, {"brightcyan", 96}, {"brightwhite", 97},
    {"bold", 1}, {"dim", 2}, {"italic", 3}, {"underline", 4}, {"blink", 5}, {"reverse", 7},
};

static int sgr_lookup_word(const char *w, size_t n)
{
    for (size_t i = 0; i < sizeof sgr_names / sizeof sgr_names[0]; i++)
        if (strlen(sgr_names[i].w) == n && pee_caseq(sgr_names[i].w, w, n))
            return sgr_names[i].code;
    return -1;
}

/* "bold red,blue" -> "\x1b[1;31;44m"; 认不出来返回 -1 */
static int sgr_parse(const char *spec, char *out, size_t n)
{
    char buf[160];
    int attr[8], na = 0, fg = -1, bg = -1, bad = 0;
    const char *s = spec;
    while (*s == ' ' || *s == '\t' || *s == '"') s++;
    snprintf(buf, sizeof buf, "%s", s);
    char *q = strchr(buf, '"');
    if (q) *q = 0;
    char *p = buf;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        char *w = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r') p++;
        int c = sgr_lookup_word(w, (size_t)(p - w));
        if (c < 0) { bad = 1; break; }
        if (c >= 30 && c <= 39) { if (fg < 0) fg = c; else if (bg < 0) bg = c + 10; }
        else if (c >= 90 && c <= 97) { if (fg < 0) fg = c; else if (bg < 0) bg = c + 10; }
        else if (na < 8) attr[na++] = c;
    }
    if (bad || (fg < 0 && bg < 0 && !na)) {
        snprintf(out, n, "%s", "\x1b[0m");
        return bad ? -1 : 0;
    }
    size_t k = 0;
    k += (size_t)snprintf(out + k, n - k, "\x1b[");
    for (int i = 0; i < na && k + 6 < n; i++) k += (size_t)snprintf(out + k, n - k, "%s%d", i ? ";" : "", attr[i]);
    if (fg >= 0 && k + 6 < n) k += (size_t)snprintf(out + k, n - k, "%s%d", na ? ";" : "", fg);
    if (bg >= 0 && k + 6 < n) k += (size_t)snprintf(out + k, n - k, ";%d", bg);
    if (k + 2 < n) k += (size_t)snprintf(out + k, n - k, "m");
    return 0;
}

static Syntax *syn_add(const char *name, const char *filepat)
{
    if (nsyn >= SYN_MAX) return NULL;
    Syntax *s = &syn[nsyn++];
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name && *name ? name : "any");
    s->filepat = filepat && *filepat ? xstrdup(filepat) : NULL;
    snprintf(s->cmt_sgr, sizeof s->cmt_sgr, "%s", "\x1b[3;90m");
    return s;
}

static Syntax *syn_any(void)
{
    for (int i = 0; i < nsyn; i++)
        if (!syn[i].filepat && !strcmp(syn[i].name, "any")) return &syn[i];
    return syn_add("any", NULL);
}

static int syn_rule(Syntax *s, const char *spec, const char *pat, int icase)
{
    if (!s || s->nrule >= (int)(sizeof s->rule / sizeof s->rule[0]) || !pat || !*pat) return 0;
    HlRule *r = &s->rule[s->nrule];
    memset(r, 0, sizeof *r);
    if (sgr_parse(spec, r->sgr, sizeof r->sgr) != 0) return 0;
    r->pat = xstrdup(pat);
    r->icase = icase;
    r->g = re_compile(r->pat);
    if (!r->g) return 0;
    if (icase) {
        char *lw = xstrdup(r->pat);
        for (char *c = lw; *c; c++) *c = (char)tolower((unsigned char)*c);
        r->gi = re_compile(lw);
        if (!r->gi) { free(lw); r->icase = 0; }
        else free(lw);
    }
    s->nrule++;
    return 1;
}

/* 内置默认语法: 与上一版硬编码的四色方案一致 */
static void syn_defaults(void)
{
    static const char *kc = "\\b(int|char|const|static|struct|return|if|else|for|while|switch|case|"
                            "break|continue|void|float|double|long|unsigned|sizeof|typedef|enum|union|"
                            "goto|do|volatile|extern|inline|bool|true|false|NULL)\\b";
    static const char *ksh = "\\b(if|then|else|fi|for|while|do|done|case|esac|function|in|return|local|"
                             "export|set|source|echo|exit|read)\\b";
    static const char *kpy = "\\b(def|class|import|from|as|if|elif|else|for|while|try|except|finally|with|"
                             "lambda|return|yield|pass|break|continue|and|or|not|in|is|None|True|False)\\b";
    static const char *kjs = "\\b(function|var|let|const|return|if|else|for|while|switch|case|break|"
                             "continue|new|delete|typeof|class|extends|this|import|export|from|true|false|null)\\b";
    static const char *pstr = "\"(\\\\.|[^\"])*\"|'(\\\\.|[^'])+'";
    static const char *pnum = "\\b[0-9]+(\\.[0-9]+)?\\b";
    Syntax *s;

    s = syn_add("c", "\\.(c|h|cpp|cc|hpp)$");
    if (s) {
        syn_rule(s, "green", pstr, 0);
        syn_rule(s, "italic brightblack", "//.*$", 0);
        syn_rule(s, "bold cyan", kc, 0);
        syn_rule(s, "yellow", pnum, 0);
        s->cmt_open = "/*"; s->cmt_close = "*/";
    }
    s = syn_add("sh", "\\.(sh|bash|zsh)$");
    if (s) {
        syn_rule(s, "green", pstr, 0);
        syn_rule(s, "italic brightblack", "#.*$", 0);
        syn_rule(s, "bold cyan", ksh, 0);
        syn_rule(s, "yellow", pnum, 0);
    }
    s = syn_add("py", "\\.(py|pyw)$");
    if (s) {
        syn_rule(s, "green", pstr, 0);
        syn_rule(s, "italic brightblack", "#.*$", 0);
        syn_rule(s, "bold cyan", kpy, 0);
        syn_rule(s, "yellow", pnum, 0);
        s->cmt_open = "\"\"\""; s->cmt_close = "\"\"\"";
    }
    s = syn_add("js", "\\.(js|ts|java|go|json)$");
    if (s) {
        syn_rule(s, "green", pstr, 0);
        syn_rule(s, "italic brightblack", "//.*$", 0);
        syn_rule(s, "bold cyan", kjs, 0);
        syn_rule(s, "yellow", pnum, 0);
        s->cmt_open = "/*"; s->cmt_close = "*/";
    }
}

static Syntax *syn_pick(const char *fname)
{
    if (!fname) return NULL;
    for (int i = 0; i < nsyn; i++) {
        if (!syn[i].filepat) continue;
        if (!syn[i].fileg) syn[i].fileg = re_compile(syn[i].filepat);
        if (!syn[i].fileg) continue;
        size_t bb, be;
        if (re_exec(syn[i].fileg, fname, strlen(fname), 0, &bb, &be)) return &syn[i];
    }
    for (int i = 0; i < nsyn; i++)
        if (!syn[i].filepat && syn[i].nrule) return &syn[i];
    return NULL;
}

static void hl_pick(const char *fname)
{
    const char *f = fname ? fname : "";
    if (!strcmp(hl_for, f)) return;
    snprintf(hl_for, sizeof hl_for, "%s", f);
    cur_syn = syn_pick(fname);
}

/* --- 跨行块注释: 每帧从第 0 行推到视口底, 只记"行首是否在注释里"这一个比特 --- */
static unsigned char *cm_in;
static size_t cm_cap_lines, cm_upto;

static size_t find_tok(const char *h, size_t n, size_t from, const char *t)
{
    size_t tl = strlen(t);
    if (!tl || from + tl > n) return (size_t)-1;
    for (size_t i = from; i + tl <= n; i++)
        if (h[i] == t[0] && !memcmp(h + i, t, tl)) return i;
    return (size_t)-1;
}

static int line_end_state(const Line *l, int in, const char *o, const char *c)
{
    size_t p = 0;
    size_t ol = strlen(o), cl = strlen(c);
    while (p < l->len) {
        if (in) {
            size_t e = find_tok(l->b, l->len, p, c);
            if (e == (size_t)-1) return 1;
            in = 0; p = e + cl;
        } else {
            size_t s = find_tok(l->b, l->len, p, o);
            if (s == (size_t)-1) return 0;
            in = 1; p = s + ol;
        }
    }
    return in;
}

static void cm_update(size_t upto)
{
    if (!hilite_on || !cur_syn || !cur_syn->cmt_open || !cur_syn->cmt_close || !D.n) {
        cm_upto = 0;
        return;
    }
    if (upto >= D.n) upto = D.n - 1;
    if (upto + 1 > cm_cap_lines) {
        cm_cap_lines = upto + 256;
        cm_in = xrealloc(cm_in, cm_cap_lines);
    }
    int in = 0;
    for (size_t i = 0; i <= upto; i++) {
        cm_in[i] = (unsigned char)in;
        in = line_end_state(&D.v[i], in, cur_syn->cmt_open, cur_syn->cmt_close);
    }
    cm_upto = upto;
}

static void cmt_paint(const Line *l, int in)
{
    const char *o = cur_syn->cmt_open, *c = cur_syn->cmt_close;
    size_t p = 0;
    size_t ol = strlen(o), cl = strlen(c);
    int guard = 0;
    while (p < l->len && ++guard < 4000) {
        size_t s;
        if (in) s = 0; else { s = find_tok(l->b, l->len, p, o); if (s == (size_t)-1) return; }
        size_t e = in ? find_tok(l->b, l->len, p, c) : find_tok(l->b, l->len, s + ol, c);
        size_t seg0 = s, seg1;
        if (e == (size_t)-1) { seg1 = l->len; in = 1; p = l->len; }
        else { seg1 = e + cl; in = 0; p = seg1; }
        for (size_t i = seg0; i < seg1 && i < l->len; i++) hl_mask[i] = HL_BLOCK;
        if (p >= l->len) return;
    }
}

static char *lc_line;
static size_t lc_cap;

static void hl_mark(HlRule *r, const Line *l, unsigned char slot)
{
    Prog *g = r->icase ? r->gi : r->g;
    const char *txt = l->b;
    size_t n = l->len;
    if (!g) return;
    if (r->icase) {
        if (n + 1 > lc_cap) { lc_cap = n + 512; lc_line = xrealloc(lc_line, lc_cap); }
        for (size_t i = 0; i < n; i++) lc_line[i] = (char)tolower((unsigned char)l->b[i]);
        txt = lc_line;
    }
    size_t st = 0, bb, be;
    int guard = 0;
    while (st <= n && ++guard < 4000 && re_exec(g, txt, n, st, &bb, &be)) {
        for (size_t i = bb; i < be && i < n; i++)
            if (hl_mask[i] == HL_NONE) hl_mask[i] = slot;
        st = be > bb ? be : bb + 1;
    }
}

static void hl_line(const Line *l, size_t li)
{
    if (l->len > hl_maskcap) {
        hl_maskcap = l->len + 256;
        hl_mask = xrealloc(hl_mask, hl_maskcap);
    }
    hl_len = l->len;
    if (l->len) memset(hl_mask, 0, l->len);
    if (!hilite_on || !cur_syn || !l->len) return;
    if (cur_syn->cmt_open && cur_syn->cmt_close && li <= cm_upto && cm_in)
        cmt_paint(l, cm_in[li]);
    for (int r = 0; r < cur_syn->nrule; r++)
        if (cur_syn->rule[r].sgr[0]) hl_mark(&cur_syn->rule[r], l, (unsigned char)(r + 1));
}

static const char *hl_sgr(int m)
{
    if (m == HL_BLOCK) return cur_syn ? cur_syn->cmt_sgr : "\x1b[3;90m";
    if (m > HL_NONE && cur_syn && m - 1 < cur_syn->nrule) return cur_syn->rule[m - 1].sgr;
    return "\x1b[0m";
}

static void ui_colors_init(void)
{
    /* 默认值与旧版硬编码的转义序列逐字节一致 */
    static const char *dflt[UC_N] = {
        "\x1b[1;7m", "\x1b[1;7m", "\x1b[0m", "\x1b[0m", "\x1b[7m", "\x1b[7;33m", "\x1b[1;31m",
        "\x1b[41;37m", "\x1b[42;30m"
    };
    for (int i = 0; i < UC_N; i++) snprintf(uisgr[i], sizeof uisgr[i], "%s", dflt[i]);
}

static int ui_slot_of(const char *name)
{
    if (pee_casew(name, "titlecolor")) return UC_TITLE;
    if (pee_casew(name, "functioncolor") || pee_casew(name, "keycolor")) return UC_BAR;
    if (pee_casew(name, "statuscolor") || pee_casew(name, "numbercolor")) return UC_STATUS;
    if (pee_casew(name, "promptcolor")) return UC_PROMPT;
    if (pee_casew(name, "selectedcolor")) return UC_SEL;
    if (pee_casew(name, "spotlightcolor")) return UC_SPOT;
    if (pee_casew(name, "errorcolor")) return UC_ERR;
    if (pee_casew(name, "diffoldcolor") || pee_casew(name, "oldcolor")) return UC_DIRO;
    if (pee_casew(name, "diffnewcolor") || pee_casew(name, "newcolor")) return UC_DIRN;
    /* 还没实现的配色项照单收下(-2), 免得老 nanorc 报错 */
    if (pee_casew(name, "scrollercolor") || pee_casew(name, "stripecolor") ||
        pee_casew(name, "minicolor") || pee_casew(name, "whitespacecolor")) return -2;
    return -1;
}


/* ====================== 搜索/替换 ====================== */
/* set casesensitive off 时: 查询串里没有大写字母就忽略大小写(智能大小写) */
static int smart_ci(const char *q)
{
    if (casesens) return 0;
    for (const char *p = q; *p; p++)
        if (*p >= 'A' && *p <= 'Z') return 0;
    return 1;
}

static int line_has_at(const Line *l, size_t i, const char *q, size_t ql, int ci)
{
    if (i + ql > l->len) return 0;
    for (size_t k = 0; k < ql; k++) {
        unsigned char a = (unsigned char)l->b[i + k], b = (unsigned char)q[k];
        if (ci) {
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        }
        if (a != b) return 0;
    }
    return 1;
}

static int find_text(Pos from, const char *q, int back, Pos *out, int *wrapped)
{
    size_t ql = strlen(q);
    if (!ql) return 0;
    if (use_regex) {
        Prog *g = re_compile(q);
        if (!g) return 0;
        int hit = 0;
        Pos best = P(0, 0);
        size_t bb = 0, be = 0;
        if (!back) {
            for (size_t li = from.line; li < D.n && !hit; li++) {
                size_t st = li == from.line ? from.off : 0;
                if (st > D.v[li].len) continue;
                if (re_exec(g, D.v[li].b, D.v[li].len, st, &bb, &be)) { best = P(li, bb); hit = 1; }
            }
            if (!hit) {
                for (size_t li = 0; li < D.n && !hit; li++)
                    if (re_exec(g, D.v[li].b, D.v[li].len, 0, &bb, &be)) { best = P(li, bb); hit = 1; *wrapped = 1; }
            }
        } else {
            for (size_t li = 0; li < D.n; li++) {
                size_t lim = li == from.line ? from.off : D.v[li].len + 1;
                if (lim > D.v[li].len + 1) lim = D.v[li].len + 1;
                size_t st = 0;
                while (st < lim && re_exec(g, D.v[li].b, D.v[li].len, st, &bb, &be)) {
                    if (bb >= lim) break;
                    best = P(li, bb); hit = 1;
                    st = be > bb ? be : bb + 1;
                }
            }
            if (!hit) {
                for (size_t li = 0; li < D.n; li++) {
                    size_t st = 0;
                    while (st <= D.v[li].len && re_exec(g, D.v[li].b, D.v[li].len, st, &bb, &be)) {
                        best = P(li, bb); hit = 1;
                        st = be > bb ? be : bb + 1;
                    }
                }
                if (hit) *wrapped = 1;
            }
        }
        re_free(g);
        if (hit) { *out = best; return 1; }
        return 0;
    }
    if (!back) {
        int ci = smart_ci(q);
        for (size_t li = from.line; li < D.n; li++) {
            size_t start = li == from.line ? from.off : 0;
            if (start > D.v[li].len) continue;
            if (ci) {
                for (size_t i = start; i + ql <= D.v[li].len; i++)
                    if (line_has_at(&D.v[li], i, q, ql, 1)) { *out = P(li, i); return 1; }
                continue;
            }
            const char *p = strstr(D.v[li].b + start, q);
            if (p) { *out = P(li, (size_t)(p - D.v[li].b)); return 1; }
        }
        for (size_t li = 0; li < D.n; li++) {
            if (ci) {
                for (size_t i = 0; i + ql <= D.v[li].len; i++)
                    if (line_has_at(&D.v[li], i, q, ql, 1)) { *out = P(li, i); *wrapped = 1; return 1; }
                continue;
            }
            const char *p = strstr(D.v[li].b, q);
            if (p) { *out = P(li, (size_t)(p - D.v[li].b)); *wrapped = 1; return 1; }
        }
    } else {
        int ci = smart_ci(q);
        Pos best = P(0, 0);
        int found = 0;
        for (size_t li = 0; li < D.n; li++) {
            size_t limit = li == from.line ? from.off : D.v[li].len;
            for (size_t i = 0; i + ql <= limit + 1 && i <= D.v[li].len; i++)
                if (line_has_at(&D.v[li], i, q, ql, ci)) { best = P(li, i); found = 1; }
        }
        if (found) { *out = best; return 1; }
        found = 0;
        for (size_t li = 0; li < D.n; li++)
            for (size_t i = 0; i + ql <= D.v[li].len; i++)
                if (line_has_at(&D.v[li], i, q, ql, ci)) { best = P(li, i); found = 1; }
        if (found) { *out = best; *wrapped = 1; return 1; }
    }
    return 0;
}

/* 位置往后推进一个字符(跨行) */
static Pos pos_advance(Pos p)
{
    if (p.off < D.v[p.line].len)
        p.off += (size_t)u8_seqlen((unsigned char)D.v[p.line].b[p.off]);
    else if (p.line + 1 < D.n) { p.line++; p.off = 0; }
    return p;
}

static void search_again(int back)
{
    if (!searchbuf.len) { set_msg("还没有搜索词"); return; }
    Pos hit;
    int wrapped = 0;
    Pos probe = back ? D.cur : pos_advance(D.cur);
    if (find_text(probe, searchbuf.b, back, &hit, &wrapped)) {
        D.cur = hit;
        D.mark = NO_MARK();
        set_msg(wrapped ? "\"%s\" 找到(回绕)" : "\"%s\" 找到了", searchbuf.b);
    } else set_msg("\"%s\" 没找到", searchbuf.b);
}

static void do_search(Term *t, int back)
{
    char buf[256];
    int cancelled;
    buf[0] = 0;
    search_anchor = D.cur;
    prompt_notify = back ? NULL : inc_notify;
    prompt_hist = HIST_SEARCH;
    if (prompt(t, back ? "反向搜索: " : "搜索: ", buf, sizeof buf,
               "", &cancelled) < 0) {
        prompt_notify = NULL;
        quitting = 1;
        return;
    }
    prompt_notify = NULL;
    if (cancelled) { D.cur = search_anchor; set_msg("取消了"); return; }
    int repeat = !buf[0] ||
                 (searchbuf.len && strcmp(buf, searchbuf.b) == 0 &&
                  (size_t)strlen(buf) == searchbuf.len);
    if (!buf[0]) {
        if (!searchbuf.len) { set_msg("还没有搜索词"); return; }
        strcpy(buf, searchbuf.b);
    } else {
        str_reset(&searchbuf);
        str_puts(&searchbuf, buf);
    }
    hist_add(HIST_SEARCH, buf);
    /* 重复同一个搜索词 -> 跳到下一处; 新词 -> 停在增量搜索预览到的那处 */
    Pos probe = repeat ? (back ? D.cur : pos_advance(D.cur)) : search_anchor;
    Pos hit;
    int wrapped = 0;
    if (!find_text(probe, buf, back, &hit, &wrapped)) { set_msg("\"%s\" 没找到", buf); return; }
    D.cur = hit;
    D.mark = NO_MARK();
    if (wrapped) set_msg("\"%s\" 找到(回绕)", buf);
    else set_msg("\"%s\" 找到了", buf);
}

static void do_replace(Term *t)
{
    char q[256], r[256];
    int cancelled;
    q[0] = r[0] = 0;
    prompt_hist = HIST_SEARCH;
    if (prompt(t, "查找: ", q, sizeof q, "", &cancelled) < 0) { quitting = 1; return; }
    if (cancelled || !q[0]) { set_msg("取消了"); return; }
    prompt_hist = HIST_REPLACE;
    if (prompt(t, "替换为: ", r, sizeof r, "", &cancelled) < 0) { quitting = 1; return; }
    if (cancelled) { set_msg("取消了"); return; }
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    size_t ql = strlen(q), rl = strlen(r);
    long count = 0;
    if (use_regex) {
        Prog *g = re_compile(q);
        if (!g) { set_msg("正则式无效"); return; }
        pre_edit(0);
        for (size_t li = 0; li < D.n; li++) {
            Line *l = &D.v[li];
            size_t st = 0, bb, be;
            int guard = 0;
            while (st <= l->len && ++guard < 20000 && re_exec(g, l->b, l->len, st, &bb, &be)) {
                Str rep2;
                memset(&rep2, 0, sizeof rep2);
                expand_repl(r, &rep2, l->b);
                line_delete(l, bb, be - bb);
                line_insert(l, bb, rep2.b ? rep2.b : "", rep2.len);
                st = bb + rep2.len;
                if (be == bb) st++;
                count++;
                free(rep2.b);
                if (count > 200000) break;
            }
        }
        re_free(g);
    } else {
        pre_edit(0);
        for (size_t li = 0; li < D.n; li++) {
            Line *l = &D.v[li];
            size_t i = 0;
            while (i + ql <= l->len) {
                if (!memcmp(l->b + i, q, ql)) {
                    line_delete(l, i, ql);
                    line_insert(l, i, r, rl);
                    i += rl;
                    count++;
                } else i++;
            }
        }
    }
    str_reset(&searchbuf);
    str_puts(&searchbuf, q);
    hist_add(HIST_SEARCH, q);
    hist_add(HIST_REPLACE, r);
    if (count) { D.modified = 1; clamp_cursor(); set_msg("替换了 %ld 处", count); }
    else set_msg("\"%s\" 没找到", q);
}

/* 逐个确认的替换: 命中后高亮并问 y/n/a/c/q */
static void do_replace_each(Term *t)
{
    char q[256], r[256];
    int cancelled;
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    q[0] = r[0] = 0;
    if (prompt(t, "逐个替换, 查找: ", q, sizeof q, "", &cancelled) < 0) { quitting = 1; return; }
    if (cancelled || !q[0]) { set_msg("取消了"); return; }
    if (prompt(t, "替换为: ", r, sizeof r, "", &cancelled) < 0) { quitting = 1; return; }
    if (cancelled) { set_msg("取消了"); return; }
    hist_add(HIST_SEARCH, q);
    hist_add(HIST_REPLACE, r);
    Prog *g = NULL;
    size_t ql = strlen(q);
    if (use_regex) {
        g = re_compile(q);
        if (!g) { set_msg("正则式无效"); return; }
    }
    long done = 0, skip = 0;
    int all = 0, pos = 0;
    Pos cur = D.cur;
    for (;;) {
        Pos hit;
        size_t hb = 0, he = 0;
        int found = 0;
        for (size_t li = cur.line; li < D.n && !found; li++) {
            size_t st = li == cur.line ? cur.off : 0;
            if (use_regex) {
                if (st <= D.v[li].len && re_exec(g, D.v[li].b, D.v[li].len, st, &hb, &he)) { found = 1; }
            } else {
                const char *f = st <= D.v[li].len ? strstr(D.v[li].b + st, q) : NULL;
                if (f) { hb = (size_t)(f - D.v[li].b); he = hb + ql; found = 1; }
            }
            if (found) hit = P(li, hb);
        }
        if (!found) {
            if (!pos) break;
            pos = 1;
            cur = P(0, 0);
            continue;
        }
        pos = 0;
        D.mark = hit;
        D.cur = P(hit.line, he);
        clamp_cursor();
        clamp_scroll();
        int yes = all;
        if (!all) {
            spot_on = 1;
            int k = ask_key(t, "替换这处? (Y/N/A/C/Q) ");
            spot_on = 0;
            if (k == 'q' || k == 'Q' || k == K_ESC || k == K_NONE) break;
            if (k == 'a' || k == 'A') { all = 1; yes = 1; }
            else if (k == 'y' || k == 'Y' || k == K_ENTER) yes = 1;
        }
        if (yes) {
            pre_edit(0);
            Str rep2;
            memset(&rep2, 0, sizeof rep2);
            if (use_regex) expand_repl(r, &rep2, D.v[hit.line].b);
            else str_add(&rep2, r, strlen(r));
            line_delete(&D.v[hit.line], hb, he - hb);
            line_insert(&D.v[hit.line], hb, rep2.b ? rep2.b : "", rep2.len);
            free(rep2.b);
            D.mark = NO_MARK();
            D.cur = P(hit.line, hb + rep2.len);
            done++;
            t->full = 1;
        } else {
            D.mark = NO_MARK();
            skip++;
        }
        cur = D.cur;
        if (he == hb && cur.off == hb) {
            cur = pos_advance(cur);
        }
    }
    if (g) re_free(g);
    spot_on = 0;
    D.mark = NO_MARK();
    clamp_cursor();
    t->full = 1;
    set_msg("替换了 %ld 处, 跳过 %ld 处", done, skip);
}

/* ====================== 对齐段落 ====================== */
static void justify_para(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    pre_edit(0);
    size_t a = D.cur.line, b = D.cur.line;
    while (a > 0 && D.v[a - 1].len) a--;
    while (b + 1 < D.n && D.v[b + 1].len) b++;
    if (a > b || D.v[a].len == 0) return;
    size_t limit = fillcol > 0 ? (size_t)fillcol : body_w(&T);
    if (limit > body_w(&T)) limit = body_w(&T);
    if (limit < 20) limit = 20;

    Str text;
    memset(&text, 0, sizeof text);
    for (size_t i = a; i <= b; i++) {
        if (i > a) str_putc(&text, ' ');
        str_add(&text, D.v[i].b, D.v[i].len);
    }

    Str *chunk = NULL;
    size_t nch = 0, capch = 0, i = 0;
    while (i < text.len) {
        while (i < text.len && text.b[i] == ' ') i++;
        if (i >= text.len) break;
        size_t start = i, cells = 0, lastsp = (size_t)-1, cut = i;
        while (i < text.len) {
            int len;
            uint32_t cp = u8_decode(text.b + i, text.len - i, &len);
            size_t cw = cellw(cp, cells);
            if (!cw) cw = 1;
            if (cells + cw > limit) break;
            if (cp == ' ') lastsp = i;
            cells += cw;
            i += (size_t)len;
            cut = i;
        }
        if (i < text.len && lastsp > start) cut = lastsp;
        if (cut <= start) cut = start + 1;
        if (nch + 1 > capch) {
            capch = capch ? capch * 2 : 8;
            chunk = xrealloc(chunk, sizeof(Str) * capch);
        }
        memset(&chunk[nch], 0, sizeof(Str));
        str_add(&chunk[nch], text.b + start, cut - start);
        nch++;
        i = cut;
    }
    if (!nch) {
        chunk = xmalloc(sizeof(Str));
        memset(&chunk[0], 0, sizeof(Str));
        nch = 1;
        capch = 1;
    }

    rec_lines(-1, a, b - a + 1);
    for (size_t k = a; k <= b; k++) line_free(&D.v[k]);
    memmove(&D.v[a], &D.v[b + 1], (D.n - b - 1) * sizeof(Line));
    D.n -= (b - a + 1);
    doc_ensure(D.n + nch);
    memmove(&D.v[a + nch], &D.v[a], (D.n - a) * sizeof(Line));
    D.n += nch;
    for (size_t k = 0; k < nch; k++)
        line_set(&D.v[a + k], chunk[k].b ? chunk[k].b : "", chunk[k].len);
    rec_lines(1, a, nch);
    for (size_t k = 0; k < nch; k++) free(chunk[k].b);
    free(chunk);
    free(text.b);
    D.modified = 1;
    D.mark = NO_MARK();
    D.cur = P(a + nch - 1, D.v[a + nch - 1].len);
    clamp_cursor();
    set_msg("段落重排为 %zu 行", nch);
}

/* ====================== 配置文件 ====================== */
/* 键位别名表: alias[新键] = 该动作的默认键; -2 表示吞掉这个键 */
#define KEYSPACE 0x2100
static short alias[KEYSPACE];
static int alias_on;

static int pee_caseq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (toupper(x) != toupper(y)) return 0;
        if (!x) return 1;
    }
    return 1;
}

static int pee_casew(const char *a, const char *b)
{
    size_t n = strlen(b);
    return pee_caseq(a, b, n) && (a[n] == 0 || a[n] == ' ' || a[n] == '\t' || a[n] == '\r');
}

/* "Ctrl+Shift+Right" / "Alt+R" / "F5" / "Ins" -> 键码 */
static int keyspec(const char *txt)
{
    int ctrl = 0, alt = 0, shift = 0;
    const char *q = txt;
    for (;;) {
        if (pee_caseq(q, "ctrl+", 5)) { ctrl = 1; q += 5; continue; }
        if (pee_caseq(q, "alt+", 4)) { alt = 1; q += 4; continue; }
        if (pee_caseq(q, "meta+", 5)) { alt = 1; q += 5; continue; }
        if (pee_caseq(q, "shift+", 6)) { shift = 1; q += 6; continue; }
        break;
    }
    static const struct { const char *n; int k; } names[] = {
        { "up", K_UP }, { "down", K_DOWN }, { "left", K_LEFT }, { "right", K_RIGHT },
        { "home", K_HOME }, { "end", K_END }, { "pgup", K_PGUP }, { "prior", K_PGUP },
        { "pgdn", K_PGDN }, { "next", K_PGDN }, { "ins", K_INS }, { "insert", K_INS },
        { "del", K_DEL }, { "delete", K_DEL }, { "enter", K_ENTER }, { "return", K_ENTER },
        { "space", ' ' }, { "tab", K_TAB }, { "esc", K_ESC }, { "backspace", K_BACKSP },
        { "f1", K_F1 }, { "f2", K_F2 }, { "f3", K_F3 }, { "f4", K_F4 }, { "f5", K_F5 },
        { "f6", K_F6 }, { "f7", K_F7 }, { "f8", K_F8 }, { "f9", K_F9 }, { "f10", K_F10 },
    };
    int base = -1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (pee_casew(q, names[i].n)) base = names[i].k;
    if (base < 0 && !q[1]) base = (unsigned char)q[0];
    if (base < 0) return -1;
    if (ctrl && ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z'))) return CTRL((char)base);
    if (ctrl && base == ' ') return CTRL('2');
    if (alt && ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z'))) return META((char)base);
    if (ctrl && shift && base == K_LEFT) return K_CSSLEFT;
    if (ctrl && shift && base == K_RIGHT) return K_CSSRIGHT;
    if (shift && base == K_LEFT) return K_SLEFT;
    if (shift && base == K_RIGHT) return K_SRIGHT;
    if (shift && base == K_UP) return K_SUP;
    if (shift && base == K_DOWN) return K_SDOWN;
    if (shift && base == K_HOME) return K_SHOME;
    if (shift && base == K_END) return K_SEND;
    if (ctrl && base == K_LEFT) return K_CLEFT;
    if (ctrl && base == K_RIGHT) return K_CRIGHT;
    if (ctrl && base == K_UP) return K_CUP;
    if (ctrl && base == K_DOWN) return K_CDOWN;
    if (ctrl && base == K_HOME) return K_CHOME;
    if (ctrl && base == K_END) return K_CEND;
    if (ctrl || alt || shift) return -1;
    return base;
}

static void apply_set(const char *name, const char *val)
{
    int off = pee_casew(val, "off") || !strcmp(val, "0") || pee_casew(val, "false");
    int on = pee_casew(val, "on") || !strcmp(val, "1") || pee_casew(val, "true");
    if (pee_casew(name, "tabsize") || pee_casew(name, "tab")) {
        long n = atol(val);
        if (n > 0 && n < 64) tabstop = (size_t)n;
        return;
    }
    if (pee_casew(name, "autoindent")) { autoindent = !off; (void)on; return; }
    if (pee_casew(name, "softwrap")) { softwrap = !off; D.sc = 0; return; }
    if (pee_casew(name, "backup")) { do_backup = !off; return; }
    if (pee_casew(name, "fill")) { long n = atol(val); if (n >= 0 && n < 4000) fillcol = (int)n; return; }
    if (pee_casew(name, "snapkeep")) {
        long n = atol(val);
        if (off) n = 0;
        if (n < 0) n = 0;
        if (n > SNAP_MAX) n = SNAP_MAX;
        snapkeep = (int)n;
        return;
    }
    if (pee_casew(name, "positionlog")) { positionlog = !off; (void)on; return; }
    if (pee_casew(name, "bom")) { force_bom = !off; (void)on; return; }
    if (pee_casew(name, "execute")) { allow_exec = !off; (void)on; return; }
    if (pee_casew(name, "encoding")) {
        int e;
        if (enc_parse(val, &e)) force_enc = e;
        else set_err("看不懂的编码名: %s (可用 auto/utf-8/gbk)", val);
        return;
    }
    if (pee_casew(name, "speller")) {
        char *cut = xstrdup(val);
        char *s = cut;
        while (*s == ' ' || *s == '"') s++;
        char *e = cut + strlen(cut);
        while (e > s && (e[-1] == '"' || e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
        if (s != cut) memmove(cut, s, (size_t)(e - s));
        cut[e - s] = 0;
        free(speller_cmd);
        if (*cut) speller_cmd = cut;
        else { free(cut); speller_cmd = NULL; }
        free(speller_ok);
        speller_ok = NULL;                 /* 换了检查器, 探测结果作废 */
        return;
    }
    if (pee_casew(name, "regexp")) { use_regex = !off; return; }
    if (pee_casew(name, "syntax")) { hilite_on = !off; (void)on; return; }
    if (pee_casew(name, "smarthome")) { smarthome = !off; (void)on; return; }
    if (pee_casew(name, "cutfromcursor")) { cutfromcursor = !off; (void)on; return; }
    if (pee_casew(name, "casesensitive")) { casesens = !off; (void)on; return; }
    if (pee_casew(name, "showblanks")) { showblanks = !off; (void)on; return; }
    if (pee_casew(name, "nonewlines")) { nonewlines = !off; (void)on; return; }
    if (pee_casew(name, "atblanks")) { atblanks = !off; softwrap = atblanks ? 1 : softwrap; (void)on; return; }
    if (pee_casew(name, "whitespace")) {
        static char wst[8], wss[8];
        int l1 = 0, l2 = 0;
        const char *v = val;
        while (*v == ' ' || *v == '"') v++;
        l1 = (int)u8_seqlen((unsigned char)*v);
        if (l1 < 1 || l1 > 4) l1 = 1;
        memcpy(wst, v, (size_t)l1);
        wst[l1] = 0;
        v += l1;
        while (*v == ' ' || *v == '"') v++;
        if (*v && *v != '"') {
            l2 = (int)u8_seqlen((unsigned char)*v);
            if (l2 < 1 || l2 > 4) l2 = 1;
            memcpy(wss, v, (size_t)l2);
            wss[l2] = 0;
            ws_tab = wst;
            ws_sp = wss;
        } else if (*wst) {
            ws_tab = wst;
            ws_sp = wst;
        }
        return;
    }
    if (pee_casew(name, "matchbrackets")) {
        static char mp[32];
        const char *v = val;
        size_t k = 0;
        while (*v == ' ' || *v == '"') v++;
        while (*v && *v != '"' && k + 1 < sizeof mp) mp[k++] = *v++;
        mp[k] = 0;
        if (k >= 2 && !(k % 2)) match_pairs = mp;
        return;
    }
    if (pee_casew(name, "smarpedit") || pee_casew(name, "smoothscroll") ||
        pee_casew(name, "mouse") || pee_casew(name, "linenumbers")) return;
    {
        int slot = ui_slot_of(name);
        if (slot >= 0) {
            if (sgr_parse(val, uisgr[slot], sizeof uisgr[slot]) != 0) set_err("配色 %s 看不懂: %s", name, val);
            return;
        }
        if (slot == -2) return;                 /* 认识但还没接线的配色项 */
    }
    set_msg("配置文件不认识的说法: %s", name);
}

/* 配置行取词: 支持 "带空白的引号串"; 引号内 \" 与 \\ 转义, 其余反斜杠原样保留(正则要用 \b)
   词写进调用者给的缓冲 —— 同一个静态缓冲会让连续两次取词互相覆盖。 */
static char *cfg_next_tok(char **pp, char *out, size_t n)
{
    char *p = *pp;
    out[0] = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (!*p) { *pp = p; return NULL; }
    size_t k = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if ((*p == '\\' && (p[1] == '"' || p[1] == '\\'))) p++;
            if (k + 1 < n) out[k++] = *p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') {
            if (k + 1 < n) out[k++] = *p++;
        }
    }
    out[k] = 0;
    *pp = p;
    return out;
}

static void load_config(const char *argpath)
{
    char home_path[1024], app_path[1024];
    const char *cand[4];
    int nc = 0;
    const char *env;
    Syntax *cfg_syn = NULL;
    if (argpath) cand[nc++] = argpath;
    cand[nc++] = "pee.cfg";
    env = getenv("HOME");
    if (env) { snprintf(home_path, sizeof home_path, "%s/.peerc", env); cand[nc++] = home_path; }
    env = getenv("APPDATA");
    if (env) { snprintf(app_path, sizeof app_path, "%s\\pee.cfg", env); cand[nc++] = app_path; }
    cand[nc] = NULL;
    const char *use = NULL;
    for (int i = 0; i < nc; i++) {
        PEE_STAT_T st;
        if (PEE_STAT(cand[i], &st) == 0 && !PEE_ISDIR(st.st_mode)) { use = cand[i]; break; }
    }
    if (!use) return;
    size_t len;
    int err;
    char *data = read_file(use, &len, &err);
    if (!data) return;
    char *ln = data;
    skip_bom(&ln, &len);                    /* free() 仍用 data, 这里只挪游标 */
    int lineno = 0;
    while (ln) {
        lineno++;
        char *nl = strchr(ln, '\n');
        if (nl) *nl++ = 0;
        while (*ln == ' ' || *ln == '\t' || *ln == '\r') ln++;
        if (*ln && *ln != '#') {
            if (pee_caseq(ln, "set ", 4)) {
                char *v = ln + 4;
                char *sp = v;
                while (*sp && *sp != ' ' && *sp != '\t' && *sp != '\r') sp++;
                *sp = 0;
                char *val = sp + 1;
                while (*val == ' ' || *val == '\t') val++;
                apply_set(v, val);
            } else if (pee_caseq(ln, "bind ", 5) || pee_caseq(ln, "unbind ", 7)) {
                int unbind = pee_caseq(ln, "unbind", 6);
                char *rest = ln + (unbind ? 7 : 5);
                while (*rest == ' ' || *rest == '\t') rest++;
                char *sp = rest;
                while (*sp && *sp != ' ' && *sp != '\t' && *sp != '\r') sp++;
                *sp = 0;
                int from = keyspec(rest);
                if (from >= 0 && from < KEYSPACE) {
                    if (unbind) { alias[from] = -2; alias_on = 1; }
                    else {
                        char *to = sp + 1;
                        while (*to == ' ' || *to == '\t') to++;
                        int t = keyspec(to);
                        if (t >= 0 && t < KEYSPACE) { alias[from] = (short)t; alias_on = 1; }
                        else set_msg("配置第 %d 行动作键名看不懂: %s", lineno, to);
                    }
                } else set_msg("配置第 %d 行键名看不懂: %s", lineno, rest);
            } else if (pee_caseq(ln, "syntax ", 7)) {
                char *p = ln + 7;
                char nm[256], fp[256];
                if (!cfg_next_tok(&p, nm, sizeof nm)) nm[0] = 0;
                if (!cfg_next_tok(&p, fp, sizeof fp)) fp[0] = 0;
                cfg_syn = syn_add(nm, fp);
                if (!cfg_syn) set_err("配置第 %d 行: 语法块超过 %d 个", lineno, SYN_MAX);
            } else if (pee_caseq(ln, "end syntax", 10)) {
                cfg_syn = NULL;
            } else if (pee_caseq(ln, "icolor ", 7) || pee_caseq(ln, "color ", 6)) {
                int ic = ln[0] == 'i' || ln[0] == 'I';
                char *p = ln + (ic ? 7 : 6);
                char spec[160], pat[512];
                if (!cfg_next_tok(&p, spec, sizeof spec)) snprintf(spec, sizeof spec, "%s", "white");
                Syntax *s = cfg_syn ? cfg_syn : syn_any();
                if (!s) set_err("配置第 %d 行: 语法块太多", lineno);
                else while (cfg_next_tok(&p, pat, sizeof pat)) {
                    if (!*pat) continue;
                    if (!syn_rule(s, spec, pat, ic))
                        set_err("配置第 %d 行规则无效: %s", lineno, pat);
                }
            } else if (pee_caseq(ln, "comment ", 8)) {
                char *p = ln + 8;
                char o[256], c[256];
                Syntax *s = cfg_syn ? cfg_syn : syn_any();
                if (cfg_next_tok(&p, o, sizeof o) && cfg_next_tok(&p, c, sizeof c) && s) {
                    s->cmt_open = xstrdup(o);
                    s->cmt_close = xstrdup(c);
                } else set_err("配置第 %d 行: comment 要成对的两段", lineno);
            } else if (pee_caseq(ln, "commentcolor ", 13)) {
                char *p = ln + 13;
                Syntax *s = cfg_syn ? cfg_syn : syn_any();
                if (s && sgr_parse(p, s->cmt_sgr, sizeof s->cmt_sgr) != 0)
                    set_err("配置第 %d 行配色名看不懂: %s", lineno, p);
            } else if (pee_caseq(ln, "linter ", 7) || pee_caseq(ln, "formatter ", 10) ||
                       pee_caseq(ln, "tabgives ", 9) || pee_caseq(ln, "magic ", 6) ||
                       pee_caseq(ln, "header ", 7) || pee_caseq(ln, "extendsyntax ", 13)) {
                /* 认得但还没接线, 先咽下去, 免得抄来的 nanorc 满屏报错 */
            }
        }
        ln = nl;
    }
    free(data);
}

/* ====================== 文件 ====================== */
static char *read_file(const char *path, size_t *len, int *err)
{
    PEE_STAT_T st;
    *err = 0;
    if (PEE_STAT(path, &st) == 0) {
        if (PEE_ISDIR(st.st_mode)) { *err = 1; return NULL; }
        if (st.st_size == 0) { *len = 0; return xstrdup(""); }
    } else {
        *err = 2;                          /* 不存在 */
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { *err = 3; return NULL; }
    size_t cap = (size_t)st.st_size + 1;
    if (cap < 4096) cap = 4096;
    char *buf = xmalloc(cap);
    size_t n = 0;
    for (;;) {
        if (n + 4096 > cap) { cap = cap * 2 + 4096; buf = xrealloc(buf, cap); }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    buf[n] = 0;
    *len = n;
    return buf;
}

/* ====================== 编码 ====================== */
/* 缓冲区内部永远只存 UTF-8, 只有读写盘那一层做转换。
   判定: 能通过严格 UTF-8 校验的算 UTF-8, 否则按 GBK(CP936) —— GBK 中文的双字节序列几乎
   不可能骗过 UTF-8 校验, 合法 UTF-8 也不会被误判; 纯 ASCII 一律算 UTF-8, 不会被乱转。 */

static const char *enc_name(int e)
{
    return e == ENC_GBK ? "GBK" : "UTF-8";
}

static int utf8_strict(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need, k;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return 0;
        if (i + need >= n) return 0;
        for (k = 1; k <= need; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        if (need == 1 && (c & 0x1F) == 0) return 0;         /* 排斥过长编码 */
        i += need + 1;
    }
    return 1;
}

static int enc_guess(const char *b, size_t n)
{
    return utf8_strict(b, n) ? ENC_UTF8 : ENC_GBK;
}

static int enc_parse(const char *v, int *out)
{
    char w[32];
    size_t k = 0;
    while (*v == ' ' || *v == '"' || *v == '\'') v++;
    while (*v && *v != ' ' && *v != '"' && *v != '\'' && k + 1 < sizeof w) w[k++] = *v++;
    w[k] = 0;
    if (!k) return 0;
    if (k == 4 && pee_caseq(w, "auto", 4)) { *out = -1; return 1; }
    if ((k == 5 && pee_caseq(w, "utf-8", 5)) || (k == 4 && pee_caseq(w, "utf8", 4))) { *out = ENC_UTF8; return 1; }
    if ((k == 3 && pee_caseq(w, "gbk", 3)) || (k == 5 && pee_caseq(w, "cp936", 5)) ||
        (k == 4 && pee_caseq(w, "ansi", 4))) { *out = ENC_GBK; return 1; }
    return 0;
}

#ifdef _WIN32
static unsigned int enc_cp(int e)
{
    return e == ENC_GBK ? 936u : CP_UTF8;
}

static char *code_convert(const char *in, size_t n, int from, int to, size_t *outn, int *ok)
{
    WCHAR *w;
    char *o;
    int wn, on;
    *ok = 0;
    if (from == to) {
        o = xmalloc(n + 1);
        memcpy(o, in, n);
        o[n] = 0;
        *outn = n;
        *ok = 1;
        return o;
    }
    wn = MultiByteToWideChar(enc_cp(from), 0, in, (int)n, NULL, 0);
    if (wn <= 0) return NULL;
    w = xmalloc(sizeof(WCHAR) * (size_t)wn);
    if (MultiByteToWideChar(enc_cp(from), 0, in, (int)n, w, wn) != wn) { free(w); return NULL; }
    on = WideCharToMultiByte(enc_cp(to), 0, w, wn, NULL, 0, NULL, NULL);
    if (on <= 0) { free(w); return NULL; }
    o = xmalloc((size_t)on + 1);
    if (WideCharToMultiByte(enc_cp(to), 0, w, wn, o, on, NULL, NULL) != on) { free(w); free(o); return NULL; }
    free(w);
    o[on] = 0;
    *outn = (size_t)on;
    *ok = 1;
    return o;
}
#else
static char *code_convert(const char *in, size_t n, int from, int to, size_t *outn, int *ok)
{
    iconv_t cd;
    Str out;
    char buf[4096];
    char *srcb;
    size_t srcn;
    *ok = 0;
    if (from == to) {
        char *o = xmalloc(n + 1);
        memcpy(o, in, n);
        o[n] = 0;
        *outn = n;
        *ok = 1;
        return o;
    }
    cd = iconv_open(enc_name(to), enc_name(from));
    if (cd == (iconv_t)-1) return NULL;
    memset(&out, 0, sizeof out);
    srcb = (char *)in;
    srcn = n;
    for (;;) {
        char *dstb = buf;
        size_t dstn = sizeof buf;
        size_t got = iconv(cd, srcn ? &srcb : NULL, &srcn, &dstb, &dstn);
        str_add(&out, buf, sizeof buf - dstn);
        if (got != (size_t)-1) break;
        if (errno == EILSEQ || errno == EINVAL) {       /* 坏字节: 跳过一个继续 */
            if (!srcn) break;
            srcb++;
            srcn--;
            continue;
        }
        break;
    }
    iconv_close(cd);
    *ok = 1;
    *outn = out.len;
    return out.b ? out.b : xstrdup("");
}
#endif

static void load_buffer(char *data, size_t len)
{
    for (size_t i = 0; i < D.n; i++) line_free(&D.v[i]);
    D.n = 0;
    size_t i = 0;
    int crlf = 0;
    D.bom = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        D.bom = 1;                     /* BOM 不算正文, 不进缓冲区 */
        i = 3;
    }
    /* 编码: 显式指定优先, 否则严格校验 UTF-8, 不合格按 GBK 转成 UTF-8 再入缓冲 */
    D.enc = (force_enc >= 0) ? force_enc : enc_guess(data + i, len - i);
    if (D.enc == ENC_GBK && i == 3) D.bom = 0;         /* 有 BOM 就不会是 GBK */
    if (D.enc != ENC_UTF8) {
        size_t un = 0;
        int ok = 0;
        char *u = code_convert(data + i, len - i, ENC_GBK, ENC_UTF8, &un, &ok);
        if (ok && u) {
            free(data);
            data = u;
            len = un;
            i = 0;
        } else {
            free(u);
            D.enc = ENC_UTF8;                          /* 转不动就原样按字节留着 */
        }
    }
    for (;;) {
        size_t j = i;
        while (j < len && data[j] != '\n') j++;
        size_t n = j - i;
        if (n && data[i + n - 1] == '\r') { n--; crlf = 1; }
        doc_ensure(D.n + 1);
        line_set(&D.v[D.n++], data + i, n);
        if (j >= len) break;
        i = j + 1;
    }
    free(data);
    D.crlf = crlf;
    if (force_bom) D.bom = 1;
    D.modified = 0;
    D.cur = P(0, 0);
    D.top = 0;
    D.sc = 0;
    clamp_cursor();
}

/* 生成 name+后缀 的路径, 调用者 free */
static char *suffixed(const char *name, const char *suf)
{
    size_t n = strlen(name);
    char *p = xmalloc(n + strlen(suf) + 2);
    memcpy(p, name, n);
    strcpy(p + n, suf);
    return p;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *fi = fopen(src, "rb");
    if (!fi) return 0;
    FILE *fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); return 0; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fi)) > 0)
        if (fwrite(buf, 1, n, fo) != n) { fclose(fi); fclose(fo); return 0; }
    fclose(fi);
    return fclose(fo) == 0;
}

/* 锁文件: name#lock#, 内容随便写点便于人看 */
static char *lockpath;

static void make_lock(const char *name)
{
    char *lp = suffixed(name, "#lock#");
    FILE *f = fopen(lp, "wb");
    if (f) { fprintf(f, "pee pid=%ld\n", (long)getpid_ish()); fclose(f); lockpath = lp; }
    else free(lp);
}

static void remove_lock(void)
{
    if (lockpath) { remove(lockpath); free(lockpath); lockpath = NULL; }
}

static int write_file(const char *path)
{
    Str s;
    char *out;
    size_t outn, wn;
    memset(&s, 0, sizeof s);
    wn = D.n;
    if (nonewlines && wn > 1 && D.v[wn - 1].len == 0) wn--;    /* 文末不补换行 */
    if (D.bom && D.enc == ENC_UTF8) str_add(&s, "\xEF\xBB\xBF", 3);
    for (size_t i = 0; i < wn; i++) {
        if (D.v[i].len) str_add(&s, D.v[i].b, D.v[i].len);
        if (i + 1 < wn) str_add(&s, D.crlf ? "\r\n" : "\n", D.crlf ? 2 : 1);
    }
    out = s.b;
    outn = s.len;
    s.b = NULL;                              /* 所有权交给 out, 下面只 free 一次 */
    if (!out) { out = xstrdup(""); outn = 0; }
    if (D.enc == ENC_GBK) {                  /* 存盘时转回原来的编码 */
        size_t gn = 0;
        int ok = 0;
        char *g = code_convert(out, outn, ENC_UTF8, ENC_GBK, &gn, &ok);
        free(out);
        if (!ok || !g) return 0;
        out = g;
        outn = gn;
    }
    {
        FILE *f = fopen(path, "wb");
        if (!f) { free(out); return 0; }
        if (outn && fwrite(out, 1, outn, f) != outn) { fclose(f); free(out); return 0; }
        if (fclose(f) != 0) { free(out); return 0; }
    }
    free(out);
    return 1;
}

/* ====================== 时间旅行存档 ====================== */
/* 每次存盘顺手往 同目录/.pee/snap/ 落一份带时间戳的副本, 于是 Alt+H 能把"十分钟前的样子"
   读回来看。存的是本来要落盘的那份字节(编码/BOM/CRLF 都跟着缓冲走), 所以读回来一模一样。 */
static char snap_nm[SNAP_MAX][SNAP_NL];    /* 快照文件名(不带目录), 按时间升序 */

static void path_parent(const char *f, char *out, size_t n)
{
    const char *s = strrchr(f, '/');
    const char *b = strrchr(f, '\\');
    if (b && (!s || b > s)) s = b;
    if (!s) { snprintf(out, n, "."); return; }
    if (s == f) { snprintf(out, n, "%c", f[0]); return; }
    snprintf(out, n, "%.*s", (int)(s - f), f);
}

static void path_leaf(const char *f, char *out, size_t n)
{
    const char *s = strrchr(f, '/');
    const char *b = strrchr(f, '\\');
    if (b && (!s || b > s)) s = b;
    snprintf(out, n, "%s", s ? s + 1 : f);
}

static void mkdir_p(const char *path)
{
    char b[1024];
    char *q;
    snprintf(b, sizeof b, "%s", path);
    for (q = b + 1; *q; q++) {
        if (*q != '/' && *q != '\\') continue;
        *q = 0;
        PEE_MKDIR(b);
        *q = '/';
    }
    PEE_MKDIR(b);
}

static void snap_dir_of(char *out, size_t n)
{
    char up[1024];
    path_parent(D.fname, up, sizeof up);
    snprintf(out, n, "%s/%s", up, SNAP_SUB);
}

static void snap_stamp(char *out, size_t n)
{
    time_t now = time(NULL);
    struct tm tv;
    memset(&tv, 0, sizeof tv);
#ifdef _WIN32
    localtime_s(&tv, &now);
#else
    {
        struct tm *p = localtime(&now);
        if (p) tv = *p;
    }
#endif
    if (!strftime(out, n, "%Y%m%d-%H%M%S", &tv)) snprintf(out, n, "%015ld", (long)now);
}

static const char *snap_at(const char *nm)     /* 名字尾部的时间戳起点 */
{
    size_t L = strlen(nm);
    return L > SNAP_STL + 5 ? nm + L - 5 - SNAP_STL : nm;
}

static int cmp_nm(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* 当前文件在 .pee/snap/ 里的快照, 填进 snap_nm[] 并按时间升序; 返回个数 */
static int snap_scan(void)
{
    char dir[1024], base[SNAP_NL];
    static char ent[SNAP_MAX][SNAP_NL];
    int n, i, got = 0;
    if (!D.fname) return 0;
    path_leaf(D.fname, base, sizeof base);
    snap_dir_of(dir, sizeof dir);
    n = list_dir(dir, base, ent, SNAP_MAX);
    for (i = 0; i < n; i++) {
        size_t L = strlen(ent[i]);
        /* 名字最短是 基名 + "." + 时间戳 + ".snap" */
        if (L < strlen(base) + 1 + SNAP_STL + 5 || L >= SNAP_NL) continue;
        if (strcmp(ent[i] + L - 5, ".snap")) continue;
        snprintf(snap_nm[got], SNAP_NL, "%s", ent[i]);
        got++;
    }
    if (got > 1) qsort(snap_nm, (size_t)got, SNAP_NL, cmp_nm);
    return got;
}

/* 存盘成功后调用; 返回一句贴在状态栏后面的说明("" = 没什么可说) */
static const char *snap_make(void)
{
    char dir[1024], base[SNAP_NL], stamp[32], leaf[SNAP_NL * 2], path[2048];
    size_t bytes = 0;
    int n;
    if (snapkeep <= 0 || !D.fname || D.hist) return "";
    for (size_t i = 0; i < D.n; i++) bytes += D.v[i].len + 2;
    if (bytes > SNAP_BIG) return "文件太大, 没留快照";
    path_leaf(D.fname, base, sizeof base);
    snap_dir_of(dir, sizeof dir);
    mkdir_p(dir);
    snap_stamp(stamp, sizeof stamp);
    snprintf(leaf, sizeof leaf, "%s.%s.snap", base, stamp);
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    if (!write_file(path)) return "快照没写成";
    n = snap_scan();
    while (n > snapkeep) {
        char old[2048];
        snprintf(old, sizeof old, "%s/%s", dir, snap_nm[0]);
        remove(old);
        n--;
        memmove(&snap_nm[0], &snap_nm[1], (size_t)n * SNAP_NL);
    }
    return "并留了快照";
}

/* 未保存就退出/被终止时, 把缓冲区落到 name#save# */
static void write_recovery(void)
{
    if (!D.fname || !D.modified) return;
    char *rp = suffixed(D.fname, "#save#");
    if (write_file(rp)) fprintf(stderr, "pee: 已存恢复文件 %s\n", rp);
    else fprintf(stderr, "pee: 恢复文件写失败 %s\n", rp);
    free(rp);
}

/* ====================== 提示输入 ====================== */
static void prompt_draw(Term *t, const char *label, const char *buf, size_t len, size_t cur)
{
    Str s;
    memset(&s, 0, sizeof s);
    size_t w = (size_t)t->w;
    char p[64];
    snprintf(p, sizeof p, "\x1b[%zu;1H\x1b[K%s", status_row(t) + 1, uisgr[UC_PROMPT]);
    str_puts(&s, p);
    size_t used = 0;
    emit_text(&s, label, strlen(label), &used, w);
    for (size_t i = 0; i < len; ) {
        int sl;
        uint32_t cp = u8_decode(buf + i, len - i, &sl);
        str_add(&s, buf + i, (size_t)sl);
        used += cellw(cp, used);
        i += sl;
    }
    size_t curcol = 0;
    for (size_t i = 0; i < cur && i < len; ) {
        int sl;
        uint32_t cp = u8_decode(buf + i, len - i, &sl);
        curcol += cellw(cp, curcol);
        i += sl;
    }
    while (used < w) { str_putc(&s, ' '); used++; }
    str_puts(&s, "\x1b[m");
    snprintf(p, sizeof p, "\x1b[%zu;%zuH", status_row(t) + 1, curcol + 1);
    str_puts(&s, p);
    term_write(t, s.b, s.len);
    free(s.b);
}

/* 增量搜索回调: 从锚点起找当前输入, 找到就把光标挪过去 */
static void inc_notify(const char *text)
{
    if (!*text) { D.cur = search_anchor; return; }
    Pos hit;
    int wrapped = 0;
    if (find_text(search_anchor, text, 0, &hit, &wrapped)) {
        D.cur = hit;
        D.mark = NO_MARK();
        clamp_cursor();
        clamp_scroll();
    }
}

static int ph_idx;                    /* 当前翻到第几条历史 */

static int prompt(Term *t, const char *label, char *buf, size_t size, const char *init, int *cancelled)
{
    *cancelled = 0;
    ph_idx = 0;
    int allow_tab = prompt_tab;
    prompt_tab = 0;
    size_t len = 0;
    if (init) {
        len = strlen(init);
        if (len >= size) len = size - 1;
        memcpy(buf, init, len);
    }
    buf[len] = 0;
    size_t cur = len;
    for (;;) {
        draw_screen(t);
        prompt_draw(t, label, buf, len, cur);
        char utf8[8];
        uint32_t cp;
        int k = read_key(t, utf8, &cp);
        if (k == K_RESIZE) { term_size(t); t->full = 1; continue; }
        if (k == K_NONE) return -1;
        if (k == K_ENTER) return 0;
        if (k == K_ESC || k == CTRL('c') || k == CTRL('g')) { *cancelled = 1; buf[0] = 0; return 0; }
        if (k == K_BACKSP) {
            if (cur > 0) {
                size_t i = cur - 1;
                while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i--;
                memmove(buf + i, buf + cur, len - cur + 1);
                len -= cur - i;
                cur = i;
            }
            if (prompt_notify) prompt_notify(buf);
            continue;
        }
        if (k == K_DEL) {
            if (cur < len) {
                size_t e = cur + u8_seqlen((unsigned char)buf[cur]);
                if (e > len) e = len;
                memmove(buf + cur, buf + e, len - e + 1);
                len -= e - cur;
            }
            continue;
        }
        if (k == K_LEFT) {
            if (cur > 0) { size_t i = cur - 1; while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i--; cur = i; }
            continue;
        }
        if (k == K_RIGHT) { if (cur < len) { cur += u8_seqlen((unsigned char)buf[cur]); if (cur > len) cur = len; } continue; }
        if (k == K_HOME || k == CTRL('a')) { cur = 0; continue; }
        if (k == K_END || k == CTRL('e')) { cur = len; continue; }
        if (k == META('r')) { use_regex = !use_regex; set_msg(use_regex ? "正则搜索: 开" : "正则搜索: 关"); continue; }
        if (k == K_UP || k == K_DOWN) {
            if (prompt_hist >= 0 && hist_n[prompt_hist] > 0) {
                if (k == K_UP) { if (ph_idx < hist_n[prompt_hist]) ph_idx++; }
                else if (ph_idx > 0) ph_idx--;
                const char *h = ph_idx ? hist_pick(prompt_hist, ph_idx - 1) : "";
                len = strlen(h);
                if (len >= size) len = size - 1;
                memcpy(buf, h, len);
                buf[len] = 0;
                cur = len;
                if (prompt_notify) prompt_notify(buf);
            }
            continue;
        }
        if (k == K_PGUP || k == K_PGDN) continue;
        if (k == K_TAB) {
            if (allow_tab) {
                const char *m = complete_path(buf, size, &len, &cur);
                set_msg(m ? m : "");
            }
            continue;
        }
        if (k >= 32 || k == K_UNKNOWN) {
            int n = (int)strlen(utf8);
            if (n < 1) continue;
            if (len + (size_t)n + 1 < size) {
                memmove(buf + cur + n, buf + cur, len - cur);
                memcpy(buf + cur, utf8, (size_t)n);
                len += (size_t)n;
                cur += (size_t)n;
                buf[len] = 0;
            }
            if (prompt_notify) prompt_notify(buf);
            continue;
        }
    }
}

/* 在状态行问一句话, 读一个键 */
static int ask_key(Term *t, const char *question)
{
    draw_screen(t);
    Str s;
    memset(&s, 0, sizeof s);
    char p[64];
    snprintf(p, sizeof p, "\x1b[%zu;1H\x1b[K%s", status_row(t) + 1, uisgr[UC_PROMPT]);
    str_puts(&s, p);
    size_t used = 0;
    emit_text(&s, question, strlen(question), &used, (size_t)t->w);
    str_puts(&s, "\x1b[m");
    term_write(t, s.b, s.len);
    free(s.b);
    char utf8[8];
    uint32_t cp;
    int k = read_key(t, utf8, &cp);
    while (k == K_RESIZE) { term_size(t); t->full = 1; draw_screen(t); k = read_key(t, utf8, &cp); }
    return k;
}

/* 屏幕(0 基)行列 -> 缓冲区位置 */
static int rowcol_to_pos(int row, int col, Pos *out)
{
    if (row < TEXT_TOP) return 0;
    size_t li;
    size_t ccol = (size_t)(col > 0 ? col : 0);
    if (ccol > 0) ccol--;                        /* 去掉标记列 */
    if (softwrap) {
        size_t r = line_row0[D.top] + (size_t)row - TEXT_TOP;
        if (r >= row_total) return 0;
        li = map_line[r];
        ccol += map_col[r];
    } else {
        li = D.top + (size_t)row - TEXT_TOP;
        if (li >= D.n) return 0;
        ccol += D.sc;
    }
    if (li >= D.n) li = D.n - 1;
    out->line = li;
    out->off = line_col_to_off(&D.v[li], ccol);
    return 1;
}

static void do_mouse(Term *t)
{
    Pos q;
    if (ms_btn == 4 || ms_btn == 5) {              /* 滚轮 */
        page_move(ms_btn == 5 ? 1 : -1, 0);
        return;
    }
    if (ms_press == 0) return;                     /* 抬起忽略 */
    if (!rowcol_to_pos(ms_row, ms_col, &q)) return;
    if (ms_ctrl && ms_press == 1) {              /* Ctrl+点击 = 增/删一个光标 */
        mc_toggle_at(q);
        clamp_scroll();
        t->full = 1;
        return;
    }
    if (ms_press == 2) {                           /* 拖动 = 扩展选区 */
        if (!has_mark()) D.mark = D.cur;
    } else {
        D.mark = NO_MARK();
    }
    D.cur = q;
    D.curow = line_cells(&D.v[D.cur.line], D.cur.off);
    clamp_cursor();
    clamp_scroll();
    (void)t;
}

/* ====================== 命令 ====================== */
static void do_writeout(Term *t, int append_mode)
{
    char buf[1024];
    int cancelled;
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    buf[0] = 0;
    prompt_hist = HIST_FILE;
    prompt_tab = 1;
    if (prompt(t, "文件名称: ", buf, sizeof buf, D.fname ? D.fname : "", &cancelled) < 0) { quitting = 1; return; }
    if (cancelled || !buf[0]) { set_msg("取消了"); return; }
    PEE_STAT_T st;
    int exists = PEE_STAT(buf, &st) == 0 && PEE_ISREG(st.st_mode);
    if (exists && !append_mode) {
        int k = ask_key(t, "文件已存在, 覆盖? (Y/N) ");
        if (k == 'N' || k == 'n') { set_msg("取消了"); return; }
    }
    if (exists && append_mode) {
        /* 追加: 读入原文件内容接到缓冲区末尾 */
        size_t len;
        int err;
        char *data = read_file(buf, &len, &err);
        if (!data) { set_msg("无法读 %s", buf); return; }
        if (D.n == 1 && D.v[0].len == 0) {
            load_buffer(data, len);
            free(D.fname);
            D.fname = xstrdup(buf);
            set_msg("已读入 %zu 行%s", D.n, D.enc == ENC_GBK ? " (GBK 已转成 UTF-8 显示)" : "");
            return;
        }
        /* 把每一行接在后面 */
        pre_edit(0);
        size_t nlen = len;
        char *p = data;
        skip_bom(&p, &nlen);
        size_t i = 0;
        int first = 1;
        for (;;) {
            size_t j = i;
            while (j < nlen && p[j] != '\n') j++;
            size_t n = j - i;
            if (n && p[i + n - 1] == '\r') n--;
            if (!first) do_insert_newline();
            insert_raw(p + i, n);
            first = 0;
            if (j >= nlen) break;
            i = j + 1;
        }
        free(data);
        D.modified = 1;
    }
    if (do_backup) {
        PEE_STAT_T bs;
        if (PEE_STAT(buf, &bs) == 0 && PEE_ISREG(bs.st_mode)) {
            char *bk = suffixed(buf, "~");
            if (!copy_file(buf, bk)) set_msg("备份 %s 失败", bk);
            free(bk);
        }
    }
    hist_add(HIST_FILE, buf);
    if (write_file(buf)) {
        char *old = D.fname ? suffixed(D.fname, "#save#") : NULL;
        const char *note;
        if (old) { remove(old); free(old); }
        free(D.fname);
        D.fname = xstrdup(buf);
        D.modified = 0;
        note = snap_make();               /* 名字定下来之后再留快照, 快照跟着这个名字放 */
        D.hist = 0;                       /* 已经按这个名字落盘, 不再算"从历史读来的" */
        clamp_cursor();
        set_msg("已写入 %zu 行到 \"%s\"%s%s", D.n, buf, note && note[0] ? ", " : "",
                note && note[0] ? note : "");
    } else {
        set_msg("无法写入 \"%s\": %s", buf, strerror(errno));
    }
}

static void do_open(Term *t)
{
    char buf[1024];
    int cancelled;
    if (D.modified) {
        int k = ask_key(t, "当前缓冲区有未保存的修改, 仍要打开? (Y/N) ");
        if (k != 'Y' && k != 'y') { set_msg("取消了"); return; }
    }
    buf[0] = 0;
    prompt_hist = HIST_FILE;
    prompt_tab = 1;
    if (prompt(t, "打开文件: ", buf, sizeof buf, "", &cancelled) < 0) {
        quitting = 1;
        return;
    }
    if (cancelled || !buf[0]) { set_msg("取消了"); return; }
    size_t len;
    int err;
    char *data = read_file(buf, &len, &err);
    if (!data) {
        if (err == 1) set_msg("%s 是一个目录", buf);
        else if (err == 2) set_msg("%s 不存在", buf);
        else set_msg("无法打开 %s: %s", buf, strerror(errno));
        return;
    }
    if (nbufs == 0) { buf_store(); }
    hist_add(HIST_FILE, buf);
    load_buffer(data, len);
    free(D.fname);
    D.fname = xstrdup(buf);
    if (nbufs == 0) { bufs[0] = D; nbufs = 1; curbuf = 0; }
    history_wipe();
    t->full = 1;
    set_msg("已打开 %s (%zu 行)", buf, D.n);
    if (positionlog) {
        long pl, pc;
        if (pos_recall(buf, &pl, &pc) && pl >= 1 && (size_t)pl <= D.n) {
            go_line((size_t)pl - 1);
            D.cur.off = line_col_to_off(&D.v[D.cur.line], pc > 1 ? (size_t)pc - 1 : 0);
            clamp_cursor();
            clamp_scroll();
        }
    }
}

/* Alt+O 切换存盘编码(GBK <-> UTF-8)。缓冲区里始终是 UTF-8, 这里只改"写出去用哪种编码",
   不重读文件, 所以不会丢未保存的修改。要把已经认错编码的文件救回来, 用 --encoding= 重开。 */
static void do_toggle_enc(Term *t)
{
    (void)t;
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    D.enc = (D.enc == ENC_UTF8) ? ENC_GBK : ENC_UTF8;
    if (D.enc == ENC_GBK) D.bom = 0;             /* GBK 文件不该带 UTF-8 BOM */
    set_msg("存盘编码已切到 %s(缓冲区不动, 保存时按这个写)", enc_name(D.enc));
}

/* ---------- 多缓冲 ---------- */
static void buf_store(void)
{
    seal_op();                        /* 把在途的那一步结算进当前缓冲的撤销栈 */
    if (nbufs > 0) bufs[curbuf] = D;
}

static void buf_select(Term *t, int i)
{
    buf_store();
    curbuf = i;
    D = bufs[curbuf];
    D.batch_kind = 0;                 /* 每个缓冲各留自己的撤销历史, 切换时不再清空 */
    t->full = 1;
    set_msg("缓冲 %d/%d: %s", curbuf + 1, nbufs, D.fname ? D.fname : "[新文件]");
}

static void buf_switch(Term *t, int dir)
{
    if (nbufs < 2) { set_msg("只有一个缓冲区"); return; }
    buf_select(t, (curbuf + dir + nbufs) % nbufs);
}

static void buf_newbuf(Term *t)
{
    if (nbufs >= MAXBUFS) { set_msg("缓冲区已到 %d 个", MAXBUFS); return; }
    if (D.modified) {
        char k = ask_key(t, "当前缓冲有未保存修改, 仍要新建? (Y/N) ");
        if (k != 'Y' && k != 'y') { set_msg("取消了"); return; }
    }
    buf_store();
    Doc nd;
    memset(&nd, 0, sizeof nd);
    nd.cap = 16;
    nd.v = xmalloc(sizeof(Line) * 16);
    nd.n = 1;
    line_set(&nd.v[0], "", 0);
    nd.mark = NO_MARK();
    nd.readonly = D.readonly;
    nd.bom = force_bom;
    nd.enc = force_enc >= 0 ? force_enc : ENC_UTF8;
    bufs[nbufs] = nd;
    nbufs++;
    curbuf = nbufs - 1;
    D = nd;
    history_wipe();
    t->full = 1;
    set_msg("新建缓冲 %d/%d", curbuf + 1, nbufs);
}

static void buf_close(Term *t)
{
    if (D.modified) {
        char k = ask_key(t, "这个缓冲还没保存, 确定关闭? (Y/N) ");
        if (k != 'Y' && k != 'y') { set_msg("取消了"); return; }
    }
    seal_op();
    for (size_t i = 0; i < D.n; i++) line_free(&D.v[i]);
    free(D.v);
    free(D.fname);
    doc_free_undo(&D);
    if (curbuf < nbufs - 1)
        memmove(&bufs[curbuf], &bufs[curbuf + 1], (size_t)(nbufs - curbuf - 1) * sizeof(Doc));
    nbufs--;
    if (nbufs == 0) { quitting = 1; return; }
    if (curbuf >= nbufs) curbuf = nbufs - 1;
    D = bufs[curbuf];
    t->full = 1;
    set_msg("关闭了一个缓冲, 剩 %d 个(各缓冲的撤销历史独立)", nbufs);
}

/* ---------- 历史版本: 列表 + 挑一个读回来看 ---------- */
/* 挑中的版本开成**新缓冲**, 名字挂回原文件并标成"未保存": 这样原来那份还在别的缓冲里,
   要看清楚再决定 Ctrl+S(会问覆盖)还是 Ctrl+W 直接关掉。 */
static int snap_open(Term *t, const char *path, const char *stamp)
{
    size_t len;
    int err;
    char *data = read_file(path, &len, &err);
    Doc nd;
    if (!data) { set_msg("读不了快照 %s", path); return 0; }
    if (nbufs >= MAXBUFS) { free(data); set_msg("缓冲区已满(%d 个)", MAXBUFS); return 0; }
    buf_store();
    memset(&nd, 0, sizeof nd);
    nd.cap = 16;
    nd.v = xmalloc(sizeof(Line) * 16);
    nd.n = 1;
    line_set(&nd.v[0], "", 0);
    nd.mark = NO_MARK();
    nd.bom = 0;
    nd.enc = ENC_UTF8;
    nd.readonly = D.readonly;               /* 只读缓冲翻出来的历史版本也不许顺手存 */
    nd.fname = D.fname ? xstrdup(D.fname) : NULL;      /* 挂原文件名, 存盘时才要显式确认 */
    bufs[nbufs] = nd;
    nbufs++;
    curbuf = nbufs - 1;
    D = bufs[curbuf];
    load_buffer(data, len);                 /* 接管 data 的所有权 */
    D.hist = 1;
    D.modified = 1;
    history_wipe();
    t->full = 1;
    set_msg("已把 %s 的版本读到新缓冲 %d/%d(存盘=覆盖现在的内容)", stamp, curbuf + 1, nbufs);
    return 1;
}

static void do_history(Term *t)
{
    char dir[1024], q[24], label[64], path[2048];
    static char ln[SNAP_MAX + 3][SNAP_NL + 64];
    static const char *lp[SNAP_MAX + 3];
    int n, i, pick;
    char first = 0;
    int cancelled;
    if (!D.fname) { set_msg("这个缓冲还没有文件名, 没有历史版本"); return; }
    if (snapkeep <= 0) { set_msg("快照是关着的(set snapkeep N 打开)"); return; }
    n = snap_scan();
    if (!n) { set_msg("还没有快照: 存一次盘就会往 %s/ 留一份", SNAP_SUB); return; }
    snap_dir_of(dir, sizeof dir);
    snprintf(ln[0], sizeof ln[0], "%s 的历史版本 %d 个(最新在前), 存于 %s/",
             D.fname, n, dir);
    for (i = 0; i < n; i++) {
        const char *st = snap_at(snap_nm[n - 1 - i]);
        snprintf(ln[i + 1], sizeof ln[0], "  %2d  %.4s-%.2s-%.2s %.2s:%.2s:%.2s  %s",
                 i + 1, st, st + 4, st + 6, st + 9, st + 11, st + 13, st);
    }
    snprintf(ln[n + 1], sizeof ln[0],
             "  敲序号(1..%d)进输入框, PgUp/PgDn 翻页, 其他键关闭", n);
    for (i = 0; i <= n + 1; i++) lp[i] = ln[i];
    pg_lines = lp;
    pg_n = n + 2;
    if (page_overlay(t, 1, &first) && first >= '0' && first <= '9') {
        q[0] = first;
        q[1] = 0;
    } else {
        set_msg("");
        return;
    }
    snprintf(label, sizeof label, "回到第几个版本(1..%d): ", n);
    if (prompt(t, label, q, sizeof q, q, &cancelled) < 0) { quitting = 1; return; }
    if (cancelled || !q[0]) { set_msg("取消了"); t->full = 1; return; }
    pick = atoi(q);
    if (pick < 1 || pick > n) { set_msg("没有第 %s 个版本", q); t->full = 1; return; }
    snprintf(path, sizeof path, "%s/%s", dir, snap_nm[n - pick]);
    t->full = 1;
    snap_open(t, path, snap_at(snap_nm[n - pick]));
}

/* 把文件内容插到光标处(nano 的 ^R 读入文件) */
static void do_insert_file(Term *t)
{
    char buf[1024];
    int cancelled;
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    buf[0] = 0;
    prompt_hist = HIST_FILE;
    prompt_tab = 1;
    if (prompt(t, "读入文件: ", buf, sizeof buf, "", &cancelled) < 0) { quitting = 1; return; }
    if (cancelled || !buf[0]) { set_msg("取消了"); return; }
    size_t len;
    int err;
    char *data = read_file(buf, &len, &err);
    if (!data) { set_msg("读不了 %s", buf); return; }
    pre_edit(0);
    delete_selection();
    size_t nlen = len;
    char *p = data;
    skip_bom(&p, &nlen);                    /* 读入带 BOM 的文件别把 BOM 插进正文 */
    size_t i = 0;
    int first = 1, lines = 0;
    for (;;) {
        size_t j = i;
        while (j < nlen && p[j] != '\n') j++;
        size_t n = j - i;
        if (n && p[i + n - 1] == '\r') n--;
        if (!first) do_insert_newline();
        if (n) insert_raw(p + i, n);
        first = 0;
        lines++;
        if (j >= nlen) break;
        i = j + 1;
    }
    free(data);
    D.modified = 1;
    clamp_cursor();
    set_msg("已读入 %s (%d 行)", buf, lines);
}

/* ====================== 外部命令 ====================== */
/* F8 运行命令并把输出插到光标处; F9 用命令输出替换选区。
   命令行里 %f 会换成引号包住的当前文件名(过滤时换成临时文件), %% 是字面 %。 */

#define EXEC_CAP ((size_t)8 * 1024 * 1024)

static FILE *pee_popen(const char *cmd)
{
#ifdef _WIN32
    return _popen(cmd, "rb");
#else
    return popen(cmd, "rb");
#endif
}

static int pee_pclose(FILE *f)
{
#ifdef _WIN32
    return _pclose(f);
#else
    int st = pclose(f);
    return st >= 0 ? st : -1;
#endif
}

/* 把文件名包成 shell 能安全接受的一个词 */
static char *shell_quote(const char *s)
{
#ifdef _WIN32
    size_t n = strlen(s);
    char *o = xmalloc(n + 3), *p = o;
    *p++ = '"';
    for (size_t i = 0; i < n; i++) if (s[i] != '"') *p++ = s[i];
    *p++ = '"';
    *p = 0;
    return o;
#else
    size_t n = strlen(s);
    char *o = xmalloc(4 * n + 3), *p = o;
    *p++ = '\'';
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') {
            *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
        } else *p++ = s[i];
    }
    *p++ = '\'';
    *p = 0;
    return o;
#endif
}

static char *expand_cmd(const char *tmpl, const char *fname)
{
    Str o;
    memset(&o, 0, sizeof o);
    char *q = fname && *fname ? shell_quote(fname) : shell_quote("pee#nofile#");
    for (const char *p = tmpl; *p; p++) {
        if (p[0] == '%' && p[1] == 'f') { str_puts(&o, q); p++; continue; }
        if (p[0] == '%' && p[1] == '%') { str_putc(&o, '%'); p++; continue; }
        str_putc(&o, *p);
    }
    free(q);
    return o.b ? o.b : xstrdup("");
}

/* 跑命令抓 stdout; 调用者 free 返回值。*ok=0 表示命令根本没跑起来 */
static char *run_capture(const char *cmd, size_t *outlen, int *code, int *ok, int *trunc)
{
    Str s;
    char buf[4096];
    size_t got;
    *outlen = 0; *code = 0; *ok = 0; *trunc = 0;
    FILE *f = pee_popen(cmd);
    if (!f) return NULL;
    memset(&s, 0, sizeof s);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (s.len + got > EXEC_CAP) { *trunc = 1; break; }
        str_add(&s, buf, got);
    }
    *code = pee_pclose(f);
    *ok = 1;
    *outlen = s.len;
    return s.b ? s.b : xstrdup("");
}

/* 命令输出按行插进缓冲(调用者已经 pre_edit 过); 顺带吃掉 \r, 结尾的换行不额外开空行 */
static void insert_out_text(const char *data, size_t n)
{
    size_t seg = 0, i = 0;
    int first = 1;
    while (i <= n) {
        while (i < n && data[i] != '\n') i++;
        size_t len = i - seg;
        if (len && data[seg + len - 1] == '\r') len--;
        int last = i >= n;
        if (last && !len) break;
        if (!first) do_insert_newline();
        if (len) insert_raw(data + seg, len);
        first = 0;
        if (last) break;
        seg = i + 1;
        i = seg;
    }
}

static void do_execute_insert(Term *t)
{
    char cmd[1024];
    int cancelled;
    if (!allow_exec) { set_err("外部命令已禁用(set execute off)"); return; }
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    cmd[0] = 0;
    prompt_hist = HIST_CMD;
    prompt_tab = 0;
    if (prompt(t, "运行命令(%f=文件名): ", cmd, sizeof cmd, "", &cancelled) < 0) {
        quitting = 1; return;
    }
    if (cancelled || !cmd[0]) { set_msg("取消了"); return; }
    hist_add(HIST_CMD, cmd);
    char *line = expand_cmd(cmd, D.fname);
    size_t n;
    int code, ok, trunc;
    char *out = run_capture(line, &n, &code, &ok, &trunc);
    free(line);
    if (!ok) { free(out); set_err("命令没能启动: %s", cmd); return; }
    if (n) {
        pre_edit(0);
        delete_selection();
        insert_out_text(out, n);
        D.modified = 1;
        D.mark = NO_MARK();
        clamp_cursor();
    }
    if (code != 0) set_msg("命令退出码 %d, 仍插入了 %zu 字节%s", code, n, trunc ? "(已截断)" : "");
    else set_msg("插入了 %zu 字节%s", n, trunc ? "(已截断)" : "");
    free(out);
    t->full = 1;
}

static void do_filter_selection(Term *t)
{
    char cmd[1024];
    int cancelled;
    const char *tmp = "pee#filt#";
    Pos a, b;
    if (!allow_exec) { set_err("外部命令已禁用(set execute off)"); return; }
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    if (!selecting(&a, &b)) { set_err("先用 Shift+方向 选要过滤的文本"); return; }
    Str sel;
    memset(&sel, 0, sizeof sel);
    collect_range(&sel, a, b);
    FILE *w = fopen(tmp, "wb");
    if (!w) { set_err("写不了临时文件 %s", tmp); free(sel.b); return; }
    if (sel.len) fwrite(sel.b, 1, sel.len, w);
    fclose(w);
    cmd[0] = 0;
    prompt_hist = HIST_CMD;
    prompt_tab = 0;
    if (prompt(t, "用命令过滤选区(%f=选区文件): ", cmd, sizeof cmd, "", &cancelled) < 0) {
        remove(tmp); quitting = 1; return;
    }
    if (cancelled || !cmd[0]) { remove(tmp); set_msg("取消了"); return; }
    hist_add(HIST_CMD, cmd);
    char *line = expand_cmd(cmd, tmp);
    size_t n;
    int code, ok, trunc;
    char *out = run_capture(line, &n, &code, &ok, &trunc);
    free(line);
    if (!ok) {
        free(out); free(sel.b); remove(tmp);
        set_err("命令没能启动: %s", cmd);
        return;
    }
    pre_edit(0);
    delete_selection();
    insert_out_text(out, n);
    D.modified = 1;
    D.mark = NO_MARK();
    clamp_cursor();
    free(sel.b);
    remove(tmp);
    if (code != 0) set_msg("命令退出码 %d, 选区换成 %zu 字节%s", code, n, trunc ? "(已截断)" : "");
    else set_msg("选区换成 %zu 字节%s", n, trunc ? "(已截断)" : "");
    free(out);
    t->full = 1;
}

/* ====================== 拼写检查 ====================== */
/* pee 不带词表, 走外部程序: 默认试 aspell, 再试 hunspell, 也可以 set speller "命令"。
   把整篇正文写进临时文件喂给它, 收集输出里的 "& 词 秩 数: 建议..." 标记行
   (aspell --pipe 与 hunspell -a 都是这个格式), 再从游标起逐个定位错词。 */

#define SPELL_TMP "pee#spell#"
#define SPELL_IGN 64
#define SPELL_MAXTYPO 512
static char *spell_ignore[SPELL_IGN];
static int spell_nign;

static int spell_ignored(const char *w)
{
    for (int i = 0; i < spell_nign; i++)
        if (!strcmp(spell_ignore[i], w)) return 1;
    return 0;
}

static void spell_add_ignore(const char *w)
{
    if (spell_ignored(w) || spell_nign >= SPELL_IGN) return;
    spell_ignore[spell_nign++] = xstrdup(w);
}

/* 跑一次"<程序> --version"式探测: 用管道抓输出, 不让它往控制台里喷字 */
static int speller_works(const char *prog)
{
    char cmd[1200];
    size_t n;
    int code, ok, trunc;
    snprintf(cmd, sizeof cmd, "%s --version 2>&1", prog);
    char *out = run_capture(cmd, &n, &code, &ok, &trunc);
    free(out);
    return ok && code == 0 && n > 0;
}

static const char *speller_pick(void)
{
    static const char *cand[2] = {
        "aspell --pipe --encoding utf-8", "hunspell -a -d en_US"
    };
    if (speller_ok) return speller_ok;
    if (speller_cmd && *speller_cmd) {
        if (speller_works(speller_cmd)) { speller_ok = xstrdup(speller_cmd); return speller_ok; }
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (speller_works(cand[i])) { speller_ok = xstrdup(cand[i]); return speller_ok; }
    }
    return NULL;
}

/* 解析 "& word rank count: a,b,c" / "# word rank count: a,b,c" */
static int spell_parse_mark(const char *line, char *word, size_t wn, char *sugg, size_t sn)
{
    const char *p = line;
    size_t k = 0;
    word[0] = sugg[0] = 0;
    if (p[0] != '&' && p[0] != '#') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && k + 1 < wn) word[k++] = *p++;
    word[k] = 0;
    if (!k) return 0;
    const char *col = strchr(p, ':');
    if (!col) return 1;
    col++;
    while (*col == ' ') col++;
    k = 0;
    while (*col && *col != ',' && *col != '\r' && *col != '\n' && k + 1 < sn) sugg[k++] = *col++;
    sugg[k] = 0;
    return 1;
}

/* 整篇正文 -> 临时文件(不带 BOM, 检查器不需要) */
static int spell_dump_buffer(void)
{
    FILE *f = fopen(SPELL_TMP, "wb");
    if (!f) return 0;
    for (size_t i = 0; i < D.n; i++) {
        if (D.v[i].len) fwrite(D.v[i].b, 1, D.v[i].len, f);
        if (i + 1 < D.n) fputs("\n", f);
    }
    fclose(f);
    return 1;
}

/* 从 from 起找下一个整词命中(区分大小写) */
static int spell_find_word(size_t from_line, size_t from_off, const char *w, Pos *out)
{
    size_t wl = strlen(w);
    if (!wl) return 0;
    for (size_t li = from_line; li < D.n; li++) {
        const Line *l = &D.v[li];
        size_t start = (li == from_line) ? from_off : 0;
        for (size_t i = start; i + wl <= l->len; i++) {
            if (l->b[i] != w[0] || memcmp(l->b + i, w, wl)) continue;
            if (i > 0 && re_is_word_at(l->b, l->len, i, 1)) continue;
            if (i + wl < l->len && re_is_word_at(l->b, l->len, i + wl, 0)) continue;
            *out = P(li, i);
            return 1;
        }
    }
    return 0;
}

static void do_spellcheck(Term *t)
{
    typedef struct { char w[128]; char s[128]; } Typo;
    char q[1600];
    const char *prog;
    Typo *typos;
    int ntypo = 0, all_mode = 0;
    long fixed = 0, passed = 0;
    if (!allow_exec) { set_err("拼写检查已禁用(set execute off)"); return; }
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    prog = speller_pick();
    if (!prog) {
        set_err("找不到拼写检查程序: 装 aspell/hunspell, 或 set speller \"命令\"");
        return;
    }
    if (!spell_dump_buffer()) { set_err("写不了临时文件 %s", SPELL_TMP); return; }
    snprintf(q, sizeof q, "%s < %s 2>&1", prog, SPELL_TMP);
    size_t n;
    int code, ok, trunc;
    char *out = run_capture(q, &n, &code, &ok, &trunc);
    remove(SPELL_TMP);
    if (!ok) { free(out); set_err("拼写检查没能启动: %s", prog); return; }
    typos = xmalloc(sizeof(Typo) * SPELL_MAXTYPO);
    for (size_t i = 0; i < n && ntypo < SPELL_MAXTYPO; ) {
        size_t j = i;
        char line[512];
        while (j < n && out[j] != '\n') j++;
        size_t tl = j - i;
        if (tl && tl < sizeof line) {
            memcpy(line, out + i, tl);
            line[tl] = 0;
            if (tl + 1 < sizeof line && line[tl - 1] == '\r') line[tl - 1] = 0;
            if (spell_parse_mark(line, typos[ntypo].w, sizeof typos[ntypo].w,
                                 typos[ntypo].s, sizeof typos[ntypo].s)) {
                if (typos[ntypo].s[0]) ntypo++;
            }
        }
        i = j + 1;
    }
    free(out);
    Pos from = D.cur;
    for (int i = 0; i < ntypo; i++) {
        Pos at;
        if (spell_ignored(typos[i].w)) continue;
        if (!spell_find_word(from.line, from.off, typos[i].w, &at) &&
            !spell_find_word(0, 0, typos[i].w, &at)) continue;
        size_t wl = strlen(typos[i].w);
        D.cur = at;
        D.mark = P(at.line, at.off + wl);
        clamp_scroll();
        t->full = 1;
        draw_screen(t);
        int k;
        if (all_mode) k = 'y';
        else {
            char ask[512];
            snprintf(ask, sizeof ask, "把 \"%s\" 换成 \"%s\"? (Y/N/A/I/Q) ", typos[i].w, typos[i].s);
            k = ask_key(t, ask);
        }
        D.mark = NO_MARK();
        if (k == 'q' || k == 'Q' || k == K_ESC || k == K_NONE) break;
        if (k == 'i' || k == 'I') { spell_add_ignore(typos[i].w); passed++; continue; }
        if (k == 'n' || k == 'N') {
            if (at.line == from.line && at.off >= from.off) from = P(at.line, at.off + wl);
            else from = P(at.line, at.off + wl);
            passed++;
            continue;
        }
        if (k == 'a' || k == 'A') all_mode = 1;
        pre_edit(0);
        line_delete(&D.v[at.line], at.off, wl);
        line_insert(&D.v[at.line], at.off, typos[i].s, strlen(typos[i].s));
        D.modified = 1;
        fixed++;
        from = P(at.line, at.off + strlen(typos[i].s));
    }
    free(typos);
    clamp_cursor();
    t->full = 1;
    if (fixed && passed) set_msg("改了 %ld 处, 跳过 %ld 处", fixed, passed);
    else if (fixed) set_msg("改了 %ld 处", fixed);
    else if (passed) set_msg("跳过了 %ld 处", passed);
    else set_msg("没发现可疑拼写%s", code ? "(检查器退出码非 0)" : "");
}

/* ====================== 补全 / 码点 / 括号 / 书签 ====================== */

static char comp_word[128];           /* 本次补全用的前缀 */
static char comp_ins[128];            /* 上一次插入的候选(用来轮换) */
static char comp_list[64][128];       /* 候选表 */
static int comp_n, comp_idx;
static int comp_active;

static void comp_reset(void)
{
    comp_n = 0;
    comp_idx = 0;
    comp_active = 0;
    comp_word[0] = 0;
    comp_ins[0] = 0;
}

static int wordbyte(unsigned char c)
{
    return c == '_' || (c < 128 && isalnum(c));
}

/* 从 end 处往前退到词首(UTF-8 安全) */
static size_t back_to_word_start(const Line *l, size_t end)
{
    size_t b = end;
    while (b > 0) {
        size_t k = b - 1;
        while (k > 0 && ((unsigned char)l->b[k] & 0xC0) == 0x80) k--;
        if (!wordbyte((unsigned char)l->b[k])) break;
        b = k;
    }
    return b;
}

static Pos bmark;                  /* 书签位置 */

/* F12 词补全: 光标前的词作前缀, 在所有缓冲里找更长的词; 再按一次换下一个候选 */
static void complete_word(Term *t)
{
    Line *l;
    size_t b, e;
    int cont = 0;
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    buf_store();
    l = &D.v[D.cur.line];
    e = D.cur.off;
    b = back_to_word_start(l, e);
    if (e == b) { comp_reset(); set_msg("光标前没有词可以补全"); return; }
    /* 光标前正好是上次插入的候选 -> 延续同一个补全序列(前缀沿用, 不再重算) */
    if (comp_active && comp_ins[0]) {
        size_t il = strlen(comp_ins);
        if (e >= il && !memcmp(l->b + e - il, comp_ins, il)) {
            cont = 1;
            b = e - il;                    /* 只替换上次插进去的那个词 */
        }
    }
    if (!cont) {
        if (e - b >= sizeof comp_word) b = e - sizeof(comp_word) + 1;
        memcpy(comp_word, l->b + b, e - b);
        comp_word[e - b] = 0;
    }
    if (!cont) {
        int bu, li;
        comp_n = 0;
        comp_idx = 0;
        for (bu = 0; bu < nbufs && comp_n < 64; bu++) {
            Doc *d = &bufs[bu];
            for (li = 0; li < (int)d->n && comp_n < 64; li++) {
                Line *w = &d->v[li];
                size_t p = 0;
                while (p < w->len) {
                    size_t s;
                    while (p < w->len && !wordbyte((unsigned char)w->b[p])) p++;
                    s = p;
                    while (p < w->len && wordbyte((unsigned char)w->b[p])) p++;
                    if (p > s && p - s > strlen(comp_word) &&
                        p - s < sizeof comp_list[0] &&
                        !memcmp(w->b + s, comp_word, strlen(comp_word))) {
                        int z, dup = 0;
                        for (z = 0; z < comp_n; z++)
                            if (strlen(comp_list[z]) == p - s &&
                                !memcmp(comp_list[z], w->b + s, p - s)) dup = 1;
                        if (!dup) {
                            memcpy(comp_list[comp_n], w->b + s, p - s);
                            comp_list[comp_n][p - s] = 0;
                            comp_n++;
                        }
                    }
                    if (p == s) p++;
                }
            }
        }
    }
    if (!comp_n) { comp_reset(); set_msg("没有找到以 \"%s\" 开头的更长词", comp_word); return; }
    if (cont) comp_idx = (comp_idx + 1) % comp_n;
    if (e - b + strlen(comp_list[comp_idx]) >= sizeof comp_ins) { comp_reset(); set_msg("词太长, 不补全了"); return; }
    pre_edit(1);
    line_delete(l, b, e - b);
    line_insert(l, b, comp_list[comp_idx], strlen(comp_list[comp_idx]));
    memcpy(comp_ins, comp_list[comp_idx], strlen(comp_list[comp_idx]) + 1);
    comp_active = 1;
    D.cur.off = b + strlen(comp_ins);
    D.modified = 1;
    clamp_cursor();
    t->full = 1;
    if (comp_n > 1) set_msg("补全 %s: %s  (再按 F12 换, 共 %d 个)", comp_word, comp_ins, comp_n);
    else set_msg("补全: %s", comp_ins);
}

/* Alt+V 插入 Unicode 码点: 支持 4e2d / U+4E2D / 0x4E2D, 空格分隔可一次插多个 */
static void insert_codepoint(Term *t)
{
    char in[256];
    int cancelled, cnt = 0;
    Str out;
    const char *p;
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    in[0] = 0;
    prompt_hist = HIST_FILE;
    prompt_tab = 0;
    if (prompt(t, "插入码点(十六进制, 空格分隔): ", in, sizeof in, "", &cancelled) < 0) {
        quitting = 1; return;
    }
    if (cancelled || !in[0]) { set_msg("取消了"); return; }
    memset(&out, 0, sizeof out);
    p = in;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        if ((p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) || *p == 'x' || *p == 'X') p += 2;
        else if (*p == 'U' || *p == 'u') { p++; if (*p == '+') p++; }
        {
            char *end = NULL;
            unsigned long v = strtoul(p, &end, 16);
            char b[8];
            int n = 0;
            if (end == p) { set_err("码点看不懂: %s", p); free(out.b); return; }
            p = end;
            if (v > 0x10FFFFUL || (v >= 0xD800UL && v <= 0xDFFFUL)) {
                set_err("码点 %#lx 超出范围", v);
                free(out.b);
                return;
            }
            if (v < 0x80) {
                b[n++] = (char)v;
            } else if (v < 0x800) {
                b[n++] = (char)(0xC0 | (v >> 6));
                b[n++] = (char)(0x80 | (v & 0x3F));
            } else if (v < 0x10000) {
                b[n++] = (char)(0xE0 | (v >> 12));
                b[n++] = (char)(0x80 | ((v >> 6) & 0x3F));
                b[n++] = (char)(0x80 | (v & 0x3F));
            } else {
                b[n++] = (char)(0xF0 | (v >> 18));
                b[n++] = (char)(0x80 | ((v >> 12) & 0x3F));
                b[n++] = (char)(0x80 | ((v >> 6) & 0x3F));
                b[n++] = (char)(0x80 | (v & 0x3F));
            }
            str_add(&out, b, (size_t)n);
            cnt++;
        }
    }
    if (!cnt) { set_msg("没认出任何码点"); free(out.b); return; }
    insert_text(out.b ? out.b : "", out.len);
    free(out.b);
    t->full = 1;
    set_msg("插入了 %d 个码点", cnt);
}

/* Alt+M 跳到配对括号 */
static void goto_match(void)
{
    const char *pairs = match_pairs;
    size_t np = strlen(pairs), oi, off;
    int isopen = 0, found = 0;
    char oc = 0, cc = 0;
    long depth = 0, guard = 0;
    size_t li = D.cur.line;
    Line *l = &D.v[li];
    for (oi = 0; oi + 1 < np; oi += 2) {
        if (D.cur.off < l->len && l->b[D.cur.off] == pairs[oi]) {
            oc = pairs[oi]; cc = pairs[oi + 1]; isopen = 1; found = 1; break;
        }
        if (D.cur.off < l->len && l->b[D.cur.off] == pairs[oi + 1]) {
            oc = pairs[oi]; cc = pairs[oi + 1]; isopen = 0; found = 1; break;
        }
    }
    if (!found && D.cur.off > 0) {
        size_t k = D.cur.off - 1;
        while (k > 0 && ((unsigned char)l->b[k] & 0xC0) == 0x80) k--;
        for (oi = 0; oi + 1 < np; oi += 2) {
            if (l->b[k] == pairs[oi]) { oc = pairs[oi]; cc = pairs[oi + 1]; isopen = 1; found = 1; D.cur.off = k; break; }
            if (l->b[k] == pairs[oi + 1]) { oc = pairs[oi]; cc = pairs[oi + 1]; isopen = 0; found = 1; D.cur.off = k; break; }
        }
    }
    if (!found) { set_msg("光标不在可配对的括号上(set matchbrackets 可改字符集)"); return; }
    off = isopen ? D.cur.off + 1 : D.cur.off;
    for (;;) {
        if (++guard > 4000000L) { set_msg("配对搜索范围太大, 放弃了"); return; }
        if (isopen) {
            if (off >= l->len) {
                if (li + 1 >= D.n) break;
                li++; l = &D.v[li]; off = 0; continue;
            }
            if (l->b[off] == cc) {
                if (!depth) {
                    D.cur = P(li, off);
                    clamp_scroll();
                    set_msg("配对的右括号: 第 %zu 行第 %zu 列", li + 1, line_cells(l, off) + 1);
                    return;
                }
                depth--;
            } else if (l->b[off] == oc) depth++;
            off++;
        } else {
            if (off == 0) {
                if (li == 0) break;
                li--; l = &D.v[li]; off = l->len; continue;
            }
            off--;
            if (l->b[off] == oc) {
                if (!depth) {
                    D.cur = P(li, off);
                    clamp_scroll();
                    set_msg("配对的左括号: 第 %zu 行第 %zu 列", li + 1, line_cells(l, off) + 1);
                    return;
                }
                depth--;
            } else if (l->b[off] == cc) depth++;
        }
    }
    set_msg("没找到配对的 %c", isopen ? cc : oc);
}

/* Alt+K 设/清书签, Alt+J 跳过去 */
static void toggle_bookmark(void)
{
    if (bmark_on && bmark.line == D.cur.line && bmark.off == D.cur.off) {
        bmark_on = 0;
        set_msg("书签已清除");
        return;
    }
    bmark = D.cur;
    bmark_on = 1;
    set_msg("书签设在第 %zu 行", bmark.line + 1);
}

static void jump_bookmark(Term *t)
{
    if (!bmark_on) { set_msg("还没设书签"); return; }
    if (bmark.line >= D.n) { bmark_on = 0; set_msg("书签那一行已经没了"); return; }
    D.cur = bmark;
    if (D.cur.off > D.v[D.cur.line].len) D.cur.off = D.v[D.cur.line].len;
    clamp_cursor();
    clamp_scroll();
    t->full = 1;
    set_msg("跳到书签: 第 %zu 行", bmark.line + 1);
}

static void delete_word_back(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    pre_edit(2);
    Pos before = D.cur;
    word_back();
    if (pos_less(D.cur, before)) cut_range(D.cur, before, 0);
}

static void delete_word_fwd(void)
{
    if (D.readonly) { set_msg("缓冲区是只读的"); return; }
    pre_edit(2);
    Pos before = D.cur;
    word_fwd();
    if (pos_less(before, D.cur)) cut_range(before, D.cur, 0);
}

static void do_gotoline(Term *t)
{
    char buf[32];
    int cancelled;
    snprintf(buf, sizeof buf, "%zu", D.cur.line + 1);
    if (prompt(t, "跳到行号(1-%zu): ", buf, sizeof buf, buf, &cancelled) < 0) { quitting = 1; return; }
    if (cancelled) { set_msg("取消了"); return; }
    long n = atol(buf);
    if (n >= 1 && (size_t)n <= D.n) { go_line((size_t)n - 1); set_msg("第 %ld 行", n); }
    else set_msg("行号应在 1-%zu", D.n);
}

/* 通用文本覆盖层: 翻页浏览 pg_lines/pg_n。any_key 非 0 时, 敲一个可见字符就结束并把
   它写进 *gotch(给版本列表当"序号首字符"用); 返回 1 表示是被字符键退出的。 */
static int page_overlay(Term *t, int any_key, char *gotch)
{
    help_mode = 1;
    help_ofs = 0;
    t->full = 1;
    set_msg("");
    if (gotch) *gotch = 0;
    char utf8[8];
    uint32_t cp;
    for (;;) {
        draw_screen(t);
        int k = read_key(t, utf8, &cp);
        int pg = (int)(text_rows(t) ? text_rows(t) : 1);
        if (k == K_RESIZE) { term_size(t); t->full = 1; continue; }
        if (k == K_PGDN || k == K_NPAGE || k == K_DOWN) {
            if (help_ofs + pg < pg_n) { help_ofs += (k == K_DOWN ? 1 : pg); t->full = 1; continue; }
            break;
        }
        if (k == K_PGUP || k == K_PPAGE || k == K_UP) {
            help_ofs = help_ofs > pg ? help_ofs - pg : 0;
            t->full = 1;
            continue;
        }
        if (k == K_NONE) break;
        if (any_key) {
            /* 可打印 ASCII 直接以字符码返回, 非 ASCII 才是 K_UNKNOWN + utf8[] */
            char c = (k >= 0x20 && k <= 0x7e) ? (char)k : utf8[0];
            if (c) {
                if (gotch) *gotch = c;
                break;
            }
        }
        if (k != K_UNKNOWN) break;
    }
    help_mode = 0;
    t->full = 1;
    return gotch && *gotch;
}

static void show_help(Term *t)
{
    pg_lines = help_lines;
    pg_n = (int)(sizeof help_lines / sizeof help_lines[0]);
    page_overlay(t, 0, NULL);
}

/* ====================== 两文件并排对照 (diff) ====================== */
/* pee -d 甲 乙: 左右两栏, 差异处左标红右标绿, n/p 在差异之间跳。这是只读视图 ——
   q 回去编辑第一个文件, Ctrl+Q 直接退。
   对齐: 先掐掉首尾的公共行, 中间用 LCS 逐行对齐(区域大到超过 512x512 就退化成按行号硬对齐,
   免得吃了 O(n*m) 的内存), 再把"删几行"和"加几行"并到同一行上, 看上去就是"改了几行"。 */
static int df_wanted;
static Doc df_b;                       /* 右边那个文件, 只读 */
static int *df_pair, *df_hunk;         /* 每显示行 = (左行号, 右行号), -1 表示这一侧没有 */
static int df_rows, df_top, df_hunkc, df_curh;
static size_t df_pw;                   /* 左栏宽度(不含分隔列) */
#define DF_LCS_MAX 512

static int df_eq(const Line *a, const Line *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->b, b->b, a->len) == 0);
}

static void df_free(void)
{
    free(df_pair);
    df_pair = NULL;
    free(df_hunk);
    df_hunk = NULL;
    df_rows = df_hunkc = df_top = 0;
    df_curh = -1;
}

static int df_row_diff(int r)
{
    int a = df_pair[2 * r], b = df_pair[2 * r + 1];
    if (a < 0 || b < 0) return 1;
    return !df_eq(&D.v[a], &df_b.v[b]);
}

static void df_build(void)
{
    int na = (int)D.n, nb = (int)df_b.n;
    int pa = 0, pb = 0, ma = na, mb = nb, tail, cap;
    int *raw, rc = 0, *out, oc = 0;
    int i, j, k;
    df_free();
    cap = na + nb + 4;
    raw = xmalloc(sizeof(int) * 2 * (size_t)cap);
    out = xmalloc(sizeof(int) * 2 * (size_t)cap);
    df_pair = xmalloc(sizeof(int) * 2 * (size_t)cap);
    df_hunk = xmalloc(sizeof(int) * 2 * (size_t)cap);
    while (pa < na && pb < nb && df_eq(&D.v[pa], &df_b.v[pb])) { pa++; pb++; }
    while (ma > pa && mb > pb && df_eq(&D.v[ma - 1], &df_b.v[mb - 1])) { ma--; mb--; }
    tail = na - ma;
    for (k = 0; k < pa; k++) { raw[2 * rc] = k; raw[2 * rc + 1] = k; rc++; }
    {
        int ml = ma - pa, nl = mb - pb;
        if (ml == 0) { for (k = 0; k < nl; k++) { raw[2 * rc] = -1; raw[2 * rc + 1] = pb + k; rc++; } }
        else if (nl == 0) { for (k = 0; k < ml; k++) { raw[2 * rc] = pa + k; raw[2 * rc + 1] = -1; rc++; } }
        else if ((long)ml * nl <= (long)DF_LCS_MAX * DF_LCS_MAX) {
            int *dp = xmalloc(sizeof(int) * (size_t)(ml + 1) * (size_t)(nl + 1));
            /* 最后一行/一列是边界(全 0), 不初始化就会读到脏内存, 对齐结果跟着抖 */
            memset(dp, 0, sizeof(int) * (size_t)(ml + 1) * (size_t)(nl + 1));
#define DP(i, j) dp[(size_t)(i) * (size_t)(nl + 1) + (size_t)(j)]
            for (i = ml - 1; i >= 0; i--)
                for (j = nl - 1; j >= 0; j--)
                    DP(i, j) = df_eq(&D.v[pa + i], &df_b.v[pb + j])
                               ? DP(i + 1, j + 1) + 1
                               : (DP(i + 1, j) >= DP(i, j + 1) ? DP(i + 1, j) : DP(i, j + 1));
            i = 0; j = 0;
            while (i < ml && j < nl) {
                if (df_eq(&D.v[pa + i], &df_b.v[pb + j])) {
                    raw[2 * rc] = pa + i; raw[2 * rc + 1] = pb + j; rc++; i++; j++;
                } else if (DP(i + 1, j) >= DP(i, j + 1)) {
                    raw[2 * rc] = pa + i; raw[2 * rc + 1] = -1; rc++; i++;
                } else {
                    raw[2 * rc] = -1; raw[2 * rc + 1] = pb + j; rc++; j++;
                }
            }
            for (; i < ml; i++) { raw[2 * rc] = pa + i; raw[2 * rc + 1] = -1; rc++; }
            for (; j < nl; j++) { raw[2 * rc] = -1; raw[2 * rc + 1] = pb + j; rc++; }
#undef DP
            free(dp);
        } else {
            for (k = 0; k < ml || k < nl; k++) {
                raw[2 * rc] = k < ml ? pa + k : -1;
                raw[2 * rc + 1] = k < nl ? pb + k : -1;
                rc++;
            }
        }
    }
    for (k = 0; k < tail; k++) { raw[2 * rc] = ma + k; raw[2 * rc + 1] = mb + k; rc++; }
    /* 把紧挨着的"只有左边"和"只有右边"两段并到同一行上 */
    for (i = 0; i < rc; ) {
        int da = 0, db = 0, m;
        while (i + da < rc && raw[2 * (i + da) + 1] < 0) da++;
        while (i + da + db < rc && raw[2 * (i + da + db)] < 0) db++;
        if (da > 0 && db > 0) {
            m = da > db ? da : db;
            for (k = 0; k < m; k++) {
                out[2 * oc] = k < da ? raw[2 * (i + k)] : -1;
                out[2 * oc + 1] = k < db ? raw[2 * (i + da + k) + 1] : -1;
                oc++;
            }
            i += da + db;
        } else {
            out[2 * oc] = raw[2 * i];
            out[2 * oc + 1] = raw[2 * i + 1];
            oc++;
            i++;
        }
    }
    free(raw);
    memcpy(df_pair, out, sizeof(int) * 2 * (size_t)oc);
    free(out);
    df_rows = oc;
    /* 连续的差异行算一处 */
    for (i = 0; i < df_rows; ) {
        if (!df_row_diff(i)) { i++; continue; }
        df_hunk[2 * df_hunkc] = i;
        df_hunk[2 * df_hunkc + 1] = i;
        while (i < df_rows && df_row_diff(i)) { df_hunk[2 * df_hunkc + 1] = i; i++; }
        df_hunkc++;
    }
}

/* 读进第二个文件: 借一次 D 走同一套 BOM/编码解码, 再整个交给 df_b */
static int df_take(const char *path)
{
    size_t len;
    int err;
    char *data = read_file(path, &len, &err);
    Doc keep;
    if (!data) {
        set_msg(err == 1 ? "%s 是一个目录" : (err == 2 ? "%s 不存在" : "读不了 %s"), path);
        return 0;
    }
    seal_op();
    keep = D;
    memset(&D, 0, sizeof D);
    D.cap = 16;
    D.v = xmalloc(sizeof(Line) * 16);
    D.n = 1;
    line_set(&D.v[0], "", 0);
    D.mark = NO_MARK();
    D.fname = xstrdup(path);
    load_buffer(data, len);                  /* 接管 data */
    df_b = D;
    df_b.readonly = 1;
    D = keep;
    df_build();
    if (!df_hunkc) set_msg("%s 和 %s 一模一样", keep.fname ? keep.fname : "[新文件]", path);
    else set_msg("%d 处差异: n 下一处 p 上一处 q 退出对照", df_hunkc);
    return 1;
}

static void df_pane(Str *r, const Doc *d, int li, size_t width, const char *sgr)
{
    size_t used = 0;
    str_puts(r, sgr);
    if (li < 0) {
        while (used < width) { str_putc(r, '~'); used++; }
        str_puts(r, "\x1b[0m");
        return;
    }
    {
        const Line *l = &d->v[li];
        size_t cell = 0, i = 0;
        while (i < l->len && used < width) {
            int len;
            uint32_t cp = u8_decode(l->b + i, l->len - i, &len);
            size_t cw = cellw(cp, cell);
            if (!cw) cw = 1;
            if (used + cw > width) break;
            if (cp == '\t') {
                size_t nx = ((cell / tabstop) + 1) * tabstop;
                size_t sp = nx > cell ? nx - cell : 1;
                if (used + sp > width) sp = width - used;
                for (size_t z = 0; z < sp; z++) str_putc(r, ' ');
                used += sp;
                cell += sp;
            } else {
                str_add(r, l->b + i, (size_t)len);
                used += cw;
                cell += cw;
            }
            i += (size_t)len;
        }
    }
    while (used < width) { str_putc(r, ' '); used++; }
    str_puts(r, "\x1b[0m");
}

static void draw_diff(Term *t)
{
    Str f;
    int w = t->w, h = t->h;
    int y;
    size_t rw;
    memset(&f, 0, sizeof f);
    df_pw = (size_t)(w >= 24 ? w / 2 - 1 : 8);
    rw = (size_t)(w - (int)df_pw - 1);
    for (y = 0; y < h; y++) {
        size_t used = 0;
        row_goto(&f, (size_t)y);
        str_puts(&f, "\x1b[0m");
        if (y == 0) {
            char ln[256];
            int last = df_top + (int)text_rows(t) - 1;
            if (last > df_rows - 1) last = df_rows - 1;
            if (last < 0) last = 0;
            snprintf(ln, sizeof ln, "  diff  左:%s  右:%s  行 %d/%d  %d 处差异",
                     D.fname ? D.fname : "[新文件]", df_b.fname ? df_b.fname : "?",
                     last + 1, df_rows, df_hunkc);
            str_puts(&f, uisgr[UC_TITLE]);
            emit_text(&f, ln, strlen(ln), &used, (size_t)w);
            while (used < (size_t)w) { str_putc(&f, ' '); used++; }
            str_puts(&f, "\x1b[0m");
            continue;
        }
        if (y == (int)status_row(t)) {
            const char *hint = "  n 下一处  p 上一处  空格/PgDn 翻页  q 回到编辑  Ctrl+Q 退出";
            str_puts(&f, uisgr[UC_STATUS]);
            emit_text(&f, hint, strlen(hint), &used, (size_t)w);
            while (used < (size_t)w) { str_putc(&f, ' '); used++; }
            str_puts(&f, "\x1b[0m");
            continue;
        }
        if (y == (int)status_row(t) + 1) {          /* 上一条消息(跳到第几处/没有更多差异) */
            str_puts(&f, msg_err ? uisgr[UC_ERR] : uisgr[UC_STATUS]);
            if (msgbuf.len) emit_text(&f, msgbuf.b, msgbuf.len, &used, (size_t)w);
            while (used < (size_t)w) { str_putc(&f, ' '); used++; }
            str_puts(&f, "\x1b[0m");
            continue;
        }
        if (y > (int)status_row(t)) {
            for (int x = 0; x < w; x++) str_putc(&f, ' ');
            continue;
        }
        {
            int r = df_top + (y - TEXT_TOP);
            if (r >= df_rows || r < 0) {
                for (int x = 0; x < w; x++) str_putc(&f, ' ');
            } else {
                int a = df_pair[2 * r], b = df_pair[2 * r + 1];
                int diff = df_row_diff(r);
                df_pane(&f, &D, a, df_pw, diff ? uisgr[UC_DIRO] : "\x1b[0m");
                str_putc(&f, '|');
                df_pane(&f, &df_b, b, rw, diff ? uisgr[UC_DIRN] : "\x1b[0m");
            }
        }
    }
    term_write(t, "\x1b[H", 3);
    term_write(t, f.b ? f.b : "", f.len);
    free(f.b);
    {
        char csi[24];
        snprintf(csi, sizeof csi, "\x1b[%zu;1H", (size_t)status_row(t) + 1);
        term_write(t, csi, strlen(csi));
    }
}

static void df_clamp(Term *t)
{
    int maxt = df_rows - (int)text_rows(t);
    if (maxt < 0) maxt = 0;
    if (df_top > maxt) df_top = maxt;
    if (df_top < 0) df_top = 0;
}

static void df_jump(Term *t, int dir)
{
    int want;
    if (df_hunkc == 0) { set_msg("没有差异可跳"); return; }
    want = df_curh + dir;
    if (want >= df_hunkc) { set_msg("后面没有差异了"); return; }
    if (want < 0) { set_msg("前面没有差异了"); return; }
    df_curh = want;
    df_top = df_hunk[2 * want] - 2;
    df_clamp(t);
    set_msg("第 %d/%d 处差异(行 %d)", want + 1, df_hunkc, df_hunk[2 * want] + 1);
}

static void do_diff(Term *t)
{
    char utf8[8];
    uint32_t cp;
    int pg = (int)text_rows(t);
    t->full = 1;
    for (;;) {
        draw_diff(t);
        int k = read_key(t, utf8, &cp);
        if (k == K_RESIZE) { term_size(t); pg = (int)text_rows(t); t->full = 1; continue; }
        if (k == K_NONE || k == CTRL('q') || k == K_F10) { quitting = 1; break; }
        if (k == 'q' || k == K_ESC) break;
        if (k == 'n' || k == K_PGDN || k == ' ') {
            if (k == 'n') df_jump(t, 1);
            else { df_top += pg; df_clamp(t); }
            continue;
        }
        if (k == 'p' || k == K_PGUP) {
            if (k == 'p') df_jump(t, -1);
            else { df_top = df_top > pg ? df_top - pg : 0; }
            continue;
        }
        if (k == K_DOWN) { df_top++; df_clamp(t); continue; }
        if (k == K_UP) { df_top = df_top > 0 ? df_top - 1 : 0; continue; }
        if (k == K_HOME || k == CTRL('h')) { df_top = 0; continue; }
        if (k == K_END) { df_top = df_rows; df_clamp(t); continue; }
        if (k == 'g') { df_jump(t, 1); continue; }
        if (k == K_F1) { show_help(t); continue; }
        if (k == K_MOUSE) {
            if (ms_btn == 4) df_top = df_top > 2 ? df_top - 2 : 0;
            else if (ms_btn == 5) { df_top += 2; df_clamp(t); }
            continue;
        }
        if (k == CTRL('s') || k == CTRL('o') || k == CTRL('w') || k == CTRL('n')) {
            set_msg("对照视图里先按 q 退出再存盘/打开");
            continue;
        }
    }
    t->full = 1;
}

static void do_exit(Term *t)
{
    if (!D.modified || D.readonly) { quitting = 1; return; }
    int k = ask_key(t, "缓冲区已修改, 保存? (Y/N/A 或 Ctrl+C 取消) ");
    if (k == 'Y' || k == 'y' || k == K_ENTER) {
        do_writeout(t, 0);
        if (!D.modified) quitting = 1;
        else { set_msg("还没保存呢!"); t->full = 1; }
    } else if (k == 'A' || k == 'a') {
        do_writeout(t, 1);
        if (!D.modified) quitting = 1;
        else set_msg("还没保存呢!");
    } else if (k == 'N' || k == 'n') {
        set_msg("丢弃了修改");
        quitting = 1;
    } else {
        set_msg("取消了");
    }
    t->full = 1;
}

/* ====================== 多光标 ====================== */
/* D.cur 是"主光标", 额外的记在 mc_pos[]; 一次按键通过 mc_each() 自上而下铺到每个光标上,
   每改完一处就把位移补偿给它下面的光标, 所以坐标不会错位, 而且整次编辑只算一步撤销。 */
typedef struct { Pos p, a; } MC;

static Pos mc_pos[MC_MAX], mc_anch[MC_MAX];
static const char *mc_txt;
static size_t mc_txtn;
static int mc_shift_pending;

static int same_pos(Pos a, Pos b)
{
    return a.line == b.line && a.off == b.off;
}

static void mc_clear(void)
{
    nmc = 0;
}

/* 主光标 + 额外光标按文档顺序合成一张表; *pprime 是主光标在表里的位置 */
static int mc_collect(MC *v, int *pprime)
{
    int n = 1, i, j, pi = 0;
    v[0].p = D.cur;
    v[0].a = D.mark;
    for (i = 0; i < nmc; i++) {
        int dup = 0;
        for (j = 0; j < n; j++)
            if (same_pos(v[j].p, mc_pos[i])) { dup = 1; break; }
        if (dup) continue;
        for (j = n; j > 0 && pos_less(mc_pos[i], v[j - 1].p); j--) v[j] = v[j - 1];
        v[j].p = mc_pos[i];
        v[j].a = mc_anch[i];
        n++;
    }
    for (i = 0; i < n; i++)
        if (same_pos(v[i].p, D.cur)) { pi = i; break; }
    *pprime = pi;
    return n;
}

static void mc_store(const MC *v, int n, int prime)
{
    int i;
    nmc = 0;
    for (i = 0; i < n; i++) {
        if (i == prime) continue;
        if (nmc >= MC_MAX) break;
        mc_pos[nmc] = v[i].p;
        mc_anch[nmc] = v[i].a;
        nmc++;
    }
    D.cur = v[prime].p;
    D.mark = v[prime].a;
}

/* 一次编辑铺到所有光标: 自上而下, 每做完一个就把改动造成的位移补偿给它下面的光标,
   这样后面光标的坐标始终对着当前缓冲区。整次编辑只算一步撤销。 */
static void mc_shift_below(MC *v, int n, int from, Pos oldp, Pos newp)
{
    long dl = (long)newp.line - (long)oldp.line;
    long doff = dl == 0 ? (long)newp.off - (long)oldp.off : 0;
    int j;
    for (j = from + 1; j < n; j++) {
        Pos *pp[2];
        int k;
        pp[0] = &v[j].p;
        pp[1] = &v[j].a;
        for (k = 0; k < 2; k++) {
            Pos *q = pp[k];
            if (q->line == (size_t)-1) continue;
            if (q->line > oldp.line) {
                if (dl) q->line = (size_t)((long)q->line + dl);
            } else if (q->line == oldp.line && doff && q->off >= oldp.off) {
                q->off = (size_t)((long)q->off + doff);
            }
            if (q->line < D.n && q->off > D.v[q->line].len) q->off = D.v[q->line].len;
        }
    }
}

static void mc_each(int kind, void (*fn)(void))
{
    static MC v[MC_MAX + 1];
    int n, pi, i;
    if (nmc == 0) { fn(); return; }
    n = mc_collect(v, &pi);
    pre_edit(kind);
    mc_depth = 1;
    for (i = 0; i < n; i++) {
        Pos oldp;
        D.cur = v[i].p;
        D.mark = v[i].a;
        oldp = v[i].p;
        fn();
        v[i].p = D.cur;
        v[i].a = D.mark;
        mc_shift_below(v, n, i, oldp, D.cur);
    }
    mc_depth = 0;
    mc_store(v, n, pi);
    D.modified = 1;
    clamp_cursor();
}

enum { MV_LEFT, MV_RIGHT, MV_UP, MV_DOWN, MV_HOME, MV_END, MV_WLEFT, MV_WRIGHT };
static int mv_kind;

static void op_move_one(void)
{
    if (mc_shift_pending) {
        if (!has_mark()) D.mark = D.cur;
    } else {
        D.mark = NO_MARK();
    }
    switch (mv_kind) {
    case MV_LEFT: move_left(); break;
    case MV_RIGHT: move_right(); break;
    case MV_UP: move_up(); break;
    case MV_DOWN: move_down(); break;
    case MV_HOME: go_home(); break;
    case MV_END: go_end(); break;
    case MV_WLEFT: word_back(); break;
    default: word_fwd(); break;
    }
}

static void mc_move(int kind, int shift)
{
    static MC v[MC_MAX + 1];
    int n, pi, i;
    if (nmc == 0) return;
    mv_kind = kind;
    mc_shift_pending = shift;
    n = mc_collect(v, &pi);
    for (i = n - 1; i >= 0; i--) {
        D.cur = v[i].p;
        D.mark = v[i].a;
        op_move_one();
        v[i].p = D.cur;
        v[i].a = D.mark;
    }
    mc_store(v, n, pi);
    mc_shift_pending = 0;
    D.curow = line_cells(&D.v[D.cur.line], D.cur.off);
    clamp_cursor();
    clamp_scroll();
}

static void clamp_pos_to_doc(Pos *q)
{
    if (q->line >= D.n) q->line = D.n ? D.n - 1 : 0;
    if (D.n && q->off > D.v[q->line].len) q->off = D.v[q->line].len;
}

/* Ctrl+点击: 已有就取消, 没有就加 */
static void mc_toggle_at(Pos q)
{
    int i;
    clamp_pos_to_doc(&q);
    if (same_pos(q, D.cur)) return;
    for (i = 0; i < nmc; i++) {
        if (same_pos(mc_pos[i], q)) {
            for (; i < nmc - 1; i++) { mc_pos[i] = mc_pos[i + 1]; mc_anch[i] = mc_anch[i + 1]; }
            nmc--;
            return;
        }
    }
    if (nmc >= MC_MAX) { set_msg("光标最多 %d 个", MC_MAX); return; }
    mc_pos[nmc] = q;
    mc_anch[nmc] = NO_MARK();
    nmc++;
    D.mark = NO_MARK();
}

/* 这一行上第 z 个额外光标的列位置(用于渲染反显) */
static int mc_cell_in(int n, const size_t *s0, const size_t *s1, size_t cell)
{
    int z;
    for (z = 0; z < n; z++)
        if (cell >= s0[z] && cell < s1[z]) return 1;
    return 0;
}

static int mc_row_cells(size_t li, size_t *cols, size_t *s0, size_t *s1, int max)
{
    int got = 0, i;
    for (i = 0; i < nmc && got < max; i++) {
        if (mc_pos[i].line != li) continue;
        cols[got] = line_cells(&D.v[li], mc_pos[i].off);
        if (mc_anch[i].line == li) {
            size_t a = line_cells(&D.v[li], mc_anch[i].off);
            s0[got] = a < cols[got] ? a : cols[got];
            s1[got] = a < cols[got] ? cols[got] : a;
        } else {
            s0[got] = cols[got];
            s1[got] = cols[got] + 1;
        }
        got++;
    }
    return got;
}

static void op_type(void)
{
    if (has_mark()) delete_selection();
    insert_raw(mc_txt, mc_txtn);
}

static void op_nl(void)
{
    if (has_mark()) delete_selection();
    do_insert_newline();
}

static void op_bs(void)
{
    backspace();
}

static void op_del(void)
{
    delete_char();
}

static void op_wbs(void)
{
    delete_word_back();
}

static void op_wdel(void)
{
    delete_word_fwd();
}

static void op_paste(void)
{
    if (has_mark()) delete_selection();
    insert_out_text(cutbuf.b ? cutbuf.b : "", cutbuf.len);
}

/* Ctrl+L: 把当前选区(没选区就光标下的词)在全文的每个出现处各生成一个光标 */
static void mc_select_occurrences(void)
{
    char needle[128];
    Pos a, b;
    size_t nl;
    int whole = 0, cnt = 0, li, guard = 0;
    size_t from_line = 0, from_off = 0;
    if (selecting(&a, &b)) {
        Str s;
        memset(&s, 0, sizeof s);
        collect_range(&s, a, b);
        if (s.len >= sizeof needle) { free(s.b); set_msg("选区太长, 不批量选"); return; }
        memcpy(needle, s.b ? s.b : "", s.len);
        needle[s.len] = 0;
        nl = s.len;
        free(s.b);
    } else {
        const Line *l = &D.v[D.cur.line];
        size_t p = D.cur.off, q = D.cur.off;
        while (p > 0 && wordbyte((unsigned char)l->b[p - 1])) p--;
        while (q < l->len && wordbyte((unsigned char)l->b[q])) q++;
        if (q <= p) { set_msg("光标处没有可选的词"); return; }
        if (q - p >= sizeof needle) q = p + sizeof(needle) - 1;
        memcpy(needle, l->b + p, q - p);
        needle[q - p] = 0;
        nl = q - p;
        whole = 1;
    }
    if (!nl) { set_msg("没有可选中的内容"); return; }
    mc_clear();
    for (li = 0; li < (int)D.n && cnt < MC_MAX; li++) {
        const Line *l = &D.v[li];
        size_t i = (li == (int)from_line) ? from_off : 0;
        while (i + nl <= l->len && ++guard < 200000) {
            size_t j;
            if (memcmp(l->b + i, needle, nl)) { i++; continue; }
            if (whole) {
                if (i > 0 && wordbyte((unsigned char)l->b[i - 1])) { i++; continue; }
                j = i + nl;
                if (j < l->len && wordbyte((unsigned char)l->b[j])) { i++; continue; }
            }
            if (cnt == 0) {
                D.cur = P((size_t)li, i + nl);
                D.mark = P((size_t)li, i);
            } else {
                mc_pos[nmc] = P((size_t)li, i + nl);
                mc_anch[nmc] = P((size_t)li, i);
                nmc++;
            }
            cnt++;
            i += nl;
        }
    }
    if (cnt <= 1) { set_msg("只找到 1 处 \"%s\"", needle); return; }
    set_msg("选中了 %d 处 \"%s\", 现在打字会同时改这些", cnt, needle);
}

static void op_del_sel(void)
{
    if (has_mark()) delete_selection();
}

/* 多光标下的复制/剪切: 每个光标的选区各算一行, 用换行拼接 */
static void mc_gather(int cut)
{
    static MC v[MC_MAX + 1];
    int n, pi, i, got = 0;
    Str all;
    memset(&all, 0, sizeof all);
    n = mc_collect(v, &pi);
    for (i = 0; i < n; i++) {
        Pos a = v[i].a, b = v[i].p;
        Str s;
        if (a.line == (size_t)-1) continue;
        if (pos_less(b, a)) { Pos x = a; a = b; b = x; }
        if (same_pos(a, b)) continue;
        memset(&s, 0, sizeof s);
        collect_range(&s, a, b);
        if (all.len) str_putc(&all, '\n');
        str_add(&all, s.b ? s.b : "", s.len);
        free(s.b);
        got++;
    }
    if (!got) { set_msg("多光标里没有可%s的选区", cut ? "剪" : "复制"); return; }
    str_reset(&cutbuf);
    str_add(&cutbuf, all.b ? all.b : "", all.len);
    clipboard_put(cutbuf.b, cutbuf.len);
    if (cut) mc_each(0, op_del_sel);
    set_msg("%s了 %d 处(用换行拼接)", cut ? "剪" : "复制", got);
}

/* 把一次只作用于 D.cur 的操作铺到所有光标上 */
static void mc_edit_typed(const char *s, size_t n)
{
    mc_txt = s;
    mc_txtn = n;
    mc_each(1, op_type);
}

/* ====================== 按键分发 ====================== */
static void handle_key(Term *t, int k, char *utf8)
{
    int vertical = 0, moved = 0, keep_batch = 0;
    if (alias_on && k >= 0 && k < KEYSPACE) {
        if (alias[k] == -2) return;
        if (alias[k] > 0) k = alias[k];
    }
    if (k != K_F12) comp_reset();          /* 换了别的键, 补全轮换就断了 */
    switch (k) {
    /* ---- 文件 ---- */
    case K_F1: case CTRL('g'): show_help(t); return;
    case K_F2: case CTRL('s'): do_writeout(t, 0); return;
    case CTRL('o'): do_open(t); return;
    case CTRL('n'): buf_newbuf(t); return;
    case CTRL('w'): buf_close(t); return;
    case META('i'): do_insert_file(t); return;
    case META(','): buf_switch(t, -1); return;
    case META('.'): buf_switch(t, 1); return;
    case K_F10: case CTRL('q'): do_exit(t); return;
    /* ---- 编辑 ---- */
    case CTRL('a'): select_all(); break;
    case CTRL('c'): if (nmc) mc_gather(0); else copy_selection(); break;
    case CTRL('x'): if (nmc) mc_gather(1); else cut_something(); break;
    case CTRL('v'):
        if (nmc) {
            char *ext = clipboard_get();
            if (ext) { str_reset(&cutbuf); str_puts(&cutbuf, ext); free(ext); }
            if (!cutbuf.len) { set_msg("剪贴板是空的"); break; }
            mc_each(0, op_paste);
            set_msg("同样内容粘到了 %d 个光标", nmc + 1);
        } else paste_cut();
        break;
    case CTRL('l'): mc_select_occurrences(); break;
    case CTRL('d'): delete_something(); break;
    case CTRL('z'): do_undo(t); return;
    case CTRL('y'): do_redo(t); return;
    case CTRL('h'): if (nmc) mc_each(0, op_wbs); else delete_word_back(); break;
    case K_CDEL: if (nmc) mc_each(0, op_wdel); else delete_word_fwd(); break;
    case CTRL('t'): do_gotoline(t); break;
    case K_F5: justify_para(); break;
    case K_F6: macro_toggle_record(); break;
    case K_F7: macro_replay(); break;
    case K_F12: complete_word(t); break;
    case META('v'): insert_codepoint(t); return;
    case META('m'): goto_match(); moved = 1; break;
    case META('k'): toggle_bookmark(); break;
    case META('h'): do_history(t); return;
    case META('j'): jump_bookmark(t); moved = 1; break;
    case K_F8: do_execute_insert(t); return;
    case K_F9: do_filter_selection(t); return;
    case K_ENTER: case 10:
        if (nmc) { mc_each(0, op_nl); }
        else insert_newline();
        break;                                    /* 回车单独一步撤销 */
    case K_TAB: tab_fill(); break;
    case META('{'): indent_block(1); break;
    case META('}'): indent_block(-1); break;
    case META('t'): convert_indent(); break;
    case META('r'): use_regex = !use_regex; set_msg(use_regex ? "正则模式: 开" : "正则模式: 关"); break;
    case META('p'): do_spellcheck(t); return;
    case META('o'): do_toggle_enc(t); break;
    case META('w'):
        softwrap = !softwrap;
        D.sc = 0;
        t->full = 1;
        set_msg(softwrap ? "软换行: 开(长行按屏幕宽度折显)" : "软换行: 关");
        break;
    case K_BACKSP:
        if (nmc) { mc_each(2, op_bs); keep_batch = 1; } else { backspace(); keep_batch = (D.batch_kind == 2); }
        break;
    case K_DEL:
        if (nmc) { mc_each(2, op_del); keep_batch = 1; } else { delete_char(); keep_batch = (D.batch_kind == 2); }
        break;
    /* ---- 查找 ---- */
    case CTRL('f'): do_search(t, 0); break;
    case K_F3: search_again(0); break;
    case K_F4: search_again(1); break;
    case CTRL('r'): do_replace(t); break;
    /* ---- 移动 ---- */
    case K_LEFT: if (nmc) mc_move(MV_LEFT, 0); else move_left(); moved = 1; break;
    case K_RIGHT: if (nmc) mc_move(MV_RIGHT, 0); else move_right(); moved = 1; break;
    case K_UP: if (nmc) mc_move(MV_UP, 0); else move_up(); vertical = 1; moved = 1; break;
    case K_DOWN: if (nmc) mc_move(MV_DOWN, 0); else move_down(); vertical = 1; moved = 1; break;
    case K_HOME: if (nmc) mc_move(MV_HOME, 0); else go_home(); moved = 1; break;
    case K_END: if (nmc) mc_move(MV_END, 0); else go_end(); moved = 1; break;
    case CTRL('e'): if (nmc) mc_move(MV_END, 0); else go_end(); moved = 1; break;
    case K_CLEFT: if (nmc) mc_move(MV_WLEFT, 0); else word_back(); moved = 1; break;
    case K_CRIGHT: if (nmc) mc_move(MV_WRIGHT, 0); else word_fwd(); moved = 1; break;
    case K_CHOME: go_line(0); moved = 1; break;
    case K_CEND: go_line(D.n - 1); D.cur.off = D.v[D.n - 1].len; moved = 1; break;
    case K_PGUP: page_move(-1, 1); vertical = 1; moved = 1; break;
    case K_PGDN: page_move(1, 1); vertical = 1; moved = 1; break;
    case K_CUP: page_move(-1, 0); vertical = 1; moved = 1; break;
    case K_CDOWN: page_move(1, 0); vertical = 1; moved = 1; break;
    case K_INS:
        overwrite = !overwrite;
        set_msg(overwrite ? "覆盖模式(打字顶掉后面的字)" : "插入模式");
        break;
    case META('l'): convert_case(1); break;
    case META('u'): convert_case(0); break;
    case META('c'): convert_case(2); break;
    case META('e'): do_replace_each(t); break;
    case K_ESC: D.mark = NO_MARK(); nmc = 0; set_msg(""); break;
    case K_RESIZE: term_size(t); t->full = 1; return;
    case K_MOUSE: do_mouse(t); break;
    /* ---- 选择 (Shift / Ctrl+Shift) ---- */
    case K_SLEFT: if (nmc) mc_move(MV_LEFT, 1); else { if (!has_mark()) D.mark = D.cur; move_left(); } break;
    case K_SRIGHT: if (nmc) mc_move(MV_RIGHT, 1); else { if (!has_mark()) D.mark = D.cur; move_right(); } break;
    case K_SUP: if (nmc) mc_move(MV_UP, 1); else { if (!has_mark()) D.mark = D.cur; move_up(); } vertical = 1; break;
    case K_SDOWN: if (nmc) mc_move(MV_DOWN, 1); else { if (!has_mark()) D.mark = D.cur; move_down(); } vertical = 1; break;
    case K_SHOME: if (nmc) mc_move(MV_HOME, 1); else { if (!has_mark()) D.mark = D.cur; go_home(); } break;
    case K_SEND: if (nmc) mc_move(MV_END, 1); else { if (!has_mark()) D.mark = D.cur; go_end(); } break;
    case K_ASSLEFT: rect_start(); move_left(); break;
    case K_ASSRIGHT: rect_start(); move_right(); break;
    case K_ASSUP: rect_start(); move_up(); vertical = 1; break;
    case K_ASSDOWN: rect_start(); move_down(); vertical = 1; break;
    case K_CSSLEFT: if (nmc) mc_move(MV_WLEFT, 1); else { if (!has_mark()) D.mark = D.cur; word_back(); } break;
    case K_CSSRIGHT: if (nmc) mc_move(MV_WRIGHT, 1); else { if (!has_mark()) D.mark = D.cur; word_fwd(); } break;
    case K_SPGUP: if (!has_mark()) D.mark = D.cur; page_move(-1, 1); vertical = 1; break;
    case K_SPGDN: if (!has_mark()) D.mark = D.cur; page_move(1, 1); vertical = 1; break;
    default:
        if (k == K_UNKNOWN && utf8[0]) {
            if (nmc) { mc_edit_typed(utf8, strlen(utf8)); keep_batch = (D.batch_kind == 1); }
            else { type_text(utf8, strlen(utf8)); keep_batch = (D.batch_kind == 1); }
        } else if (k >= 32 && k < 127) {
            char b = (char)k;
            if (nmc) { mc_edit_typed(&b, 1); keep_batch = (D.batch_kind == 1); }
            else { type_text(&b, 1); keep_batch = (D.batch_kind == 1); }
        } else if (IS_CTRL(k)) set_msg("未绑定的键: Ctrl+%c", (char)(k + 0x60));
        else if (IS_CTRL(k)) set_msg("未绑定的键: Ctrl+%c", (char)(k + 0x60));
        break;
    }
    if (!keep_batch) { seal_op(); D.batch_kind = 0; }
    if (k != CTRL('x')) last_was_cut = 0;
    if (moved) { D.mark = NO_MARK(); rect_sel = 0; }
    if (k == K_ESC) { rect_sel = 0; rect_paste_mode = 0; }
    if (!vertical) D.curow = line_cells(&D.v[D.cur.line], D.cur.off);
    clamp_cursor();
    clamp_scroll();
}

static void select_all(void)
{
    D.mark = P(0, 0);
    D.cur = P(D.n - 1, D.v[D.n - 1].len);
}

/* ====================== 主程序 ====================== */
static void usage(FILE *f)
{
    fprintf(f,
        "用法: pee [选项] [文件]   (" PEE_NAME " " PEE_VERSION ")\n"
        "  pee 文件名        编辑文件(不存在则新建)\n"
        "  pee +10 文件名    打开并定位到第 10 行\n"
        "  -R                只读模式\n"
        "  -B                保存前把旧文件备份成 名字~\n"
        "  -c 文件           指定配置文件(默认 ./pee.cfg 或家目录 .peerc)\n"
        "  -t N              制表位宽度(默认 8)\n"
        "  -w N              F5 段落重排宽度(0=按屏幕宽)\n"
        "  --no-autoindent   关闭回车自动缩进\n"
        "  --no-execute      禁用 F8/F9 外部命令\n"
        "  --softwrap        长行按屏幕宽度软换行显示\n"
        "  --bom             新建文件时写入 UTF-8 BOM(已有 BOM 的文件总是保留)\n"
        "  -E, --encoding=N  N=auto|utf-8|gbk: 强制按某种编码读写(默认自动检测)\n"
        "  -d, --diff        两个文件并排对照: pee -d 甲 乙 (n/p 跳差异, q 回编辑)\n"
        "  -H, --history     打开文件后直接进历史版本列表(平时按 Alt+H)\n"
        "  --no-snap         关掉存盘快照(默认每次存盘往 .pee/snap/ 留一份)\n"
        "  -h, --help        显示帮助\n"
        "  -V, --version     显示版本\n"
        "按键: F1 帮助  Ctrl+S 保存  Ctrl+O 打开  Ctrl+N 新建  Ctrl+Q 退出\n"
        "      Ctrl+C/X/V 复制/剪切/粘贴  Ctrl+F 查找  Ctrl+R 替换  Ctrl+D 删行\n"
        "      Shift+方向键 选择文本(复制内容不含行号)\n"
        "      Ctrl+点击 加/去光标  Ctrl+L 全文同名词各生一个光标  Esc 收起\n"
        "      F8 运行命令并插入输出  F9 用命令输出替换选区  F12 词补全  Alt+P 拼写检查\n"
        "      Alt+V 插入码点  Alt+M 跳配对括号  Alt+K/Alt+J 设/跳书签  Alt+H 历史版本  详见 F1 帮助\n");
}

int main(int argc, char **argv)
{
    const char *fname = NULL;
    const char *fname2 = NULL;
    const char *cfgpath = NULL;
    long goto_line = 0;
    int readonly = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("%s %s  (命令名 pee)\n", PEE_NAME, PEE_VERSION);
            return 0;
        }
        if (!strcmp(a, "-R") || !strcmp(a, "--read-only")) { readonly = 1; continue; }
        if (!strcmp(a, "-B") || !strcmp(a, "--backup")) { do_backup = 1; continue; }
        if (!strcmp(a, "-w") || !strcmp(a, "--fill")) { if (i + 1 < argc) fillcol = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-t") || !strcmp(a, "--tabsize")) {
            if (i + 1 < argc) { long n = atol(argv[++i]); if (n > 0 && n < 64) tabstop = (size_t)n; }
            else fprintf(stderr, "pee: %s 需要一个数字\n", a);
            continue;
        }
        if (a[0] == '-' && !strncmp(a, "--tabsize=", 10)) {
            long n = atol(a + 10); if (n > 0 && n < 64) tabstop = (size_t)n;
            continue;
        }
        if (!strcmp(a, "--no-autoindent")) { autoindent = 0; continue; }
        if (!strcmp(a, "--no-execute")) { allow_exec = 0; continue; }
        if (!strcmp(a, "--softwrap")) { softwrap = 1; continue; }
        if (!strcmp(a, "--bom")) { force_bom = 1; continue; }
        if (!strcmp(a, "--no-snap")) { snapkeep = 0; continue; }
        if (!strcmp(a, "-d") || !strcmp(a, "--diff")) { df_wanted = 1; continue; }
        if (!strcmp(a, "--history") || !strcmp(a, "-H")) { start_history = 1; continue; }
        if (!strncmp(a, "--encoding=", 11)) {
            int e;
            if (enc_parse(a + 11, &e)) force_enc = e;
            else { fprintf(stderr, "pee: 看不懂的编码名 %s\n", a + 11); return 2; }
            continue;
        }
        if (!strcmp(a, "--encoding") || !strcmp(a, "-E")) {
            int e;
            if (i + 1 < argc && enc_parse(argv[++i], &e)) force_enc = e;
            else { fprintf(stderr, "pee: --encoding 要 auto/utf-8/gbk\n"); return 2; }
            continue;
        }
        if (!strcmp(a, "-c") || !strcmp(a, "--config")) { if (i + 1 < argc) cfgpath = argv[++i]; continue; }
        if (!strcmp(a, "--autoindent")) { autoindent = 1; continue; }
        if (a[0] == '+' && a[1] >= '0' && a[1] <= '9') { goto_line = atol(a + 1); continue; }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "pee: 未知选项 %s\n", a); usage(stderr); return 2; }
        if (!fname) fname = a;
        else if (!fname2) fname2 = a;
        else fprintf(stderr, "pee: 只对照两个文件, 忽略 %s\n", a);
    }

    for (int i = 0; i < KEYSPACE; i++) alias[i] = 0;
    doc_init(fname, readonly);
    ui_colors_init();
    load_config(cfgpath);
    if (force_enc >= 0) D.enc = force_enc;   /* 配置文件里的 set encoding 要能管到新建的缓冲 */
    syn_defaults();                 /* 用户配置里的 syntax 先注册, 内置的兜底 */
    hist_load();

    if (fname) {
        size_t len;
        int err;
        char *data = read_file(fname, &len, &err);
        if (data) {
            load_buffer(data, len);
            set_msg("已读入 %zu 行%s", D.n, D.enc == ENC_GBK ? " (GBK 已转成 UTF-8 显示)" : "");        } else if (err == 1) {
            die("%s 是一个目录", fname);
        } else if (err == 2) {
            set_msg("新文件 \"%s\"", fname);
        } else {
            die("无法打开 %s: %s", fname, strerror(errno));
        }
    } else {
        set_msg("新缓冲");
    }

    bufs[0] = D;
    nbufs = 1;
    curbuf = 0;
    term_init(&T);
    if (D.fname) {
        char *rp = suffixed(D.fname, "#save#");
        PEE_STAT_T rs;
        if (PEE_STAT(rp, &rs) == 0 && PEE_ISREG(rs.st_mode)) {
            char ans = ask_key(&T, "发现未保存的恢复文件, 用它吗? (Y/N) ");
            if (ans == 'Y' || ans == 'y') {
                size_t rl; int rerr;
                char *rd = read_file(rp, &rl, &rerr);
                if (rd) { load_buffer(rd, rl); go_line(0); set_msg("已恢复 %zu 行", D.n); }
            } else remove(rp);
        }
        free(rp);
        char *lp = suffixed(D.fname, "#lock#");
        PEE_STAT_T ls;
        if (!readonly && PEE_STAT(lp, &ls) == 0) {
            char ans = ask_key(&T, "该文件已有锁文件, 可能正被另一个 pee 编辑。继续? (Y/N) ");
            if (ans != 'Y' && ans != 'y') { D.readonly = readonly = 1; set_msg("已按只读打开"); }
        }
        free(lp);
        if (!readonly) make_lock(D.fname);
    }
    if (goto_line > 0) {
        go_line((size_t)(goto_line - 1));
    } else if (positionlog && D.fname) {
        long pl, pc;
        if (pos_recall(D.fname, &pl, &pc) && pl >= 1 && (size_t)pl <= D.n) {
            go_line((size_t)pl - 1);
            D.cur.off = line_col_to_off(&D.v[D.cur.line], pc > 1 ? (size_t)pc - 1 : 0);
            clamp_cursor();
            clamp_scroll();
            set_msg("回到上次位置: 行 %ld 列 %ld", pl, pc);
        }
    }
    if (df_wanted) {
        if (!fname || !fname2) set_msg("对照要两个文件: pee -d 甲 乙 (现在只编辑一个)");
        else if (df_take(fname2)) do_diff(&T);
    }
    if (start_history && D.fname) do_history(&T);
    if (!quitting) draw_screen(&T);

    while (!quitting) {
        char utf8[8];
        uint32_t cp;
        int k = read_key(&T, utf8, &cp);
        if (k == K_NONE) break;
        if (k == K_RESIZE) { term_size(&T); T.full = 1; draw_screen(&T); continue; }
        handle_key(&T, k, utf8);
        draw_screen(&T);
    }
    if (positionlog && D.fname)
        pos_remember(D.fname, (long)D.cur.line + 1,
                     (long)line_cells(&D.v[D.cur.line], D.cur.off) + 1);
    remove_lock();
    hist_save();
    term_restore(&T);
    if (D.modified && !quitting) write_recovery();
    if (D.modified && quitting) {
        fprintf(stderr, "pee: 缓冲区未保存 (%zu 行)\n", D.n);
    }
    doc_free();
    fprintf(stderr, "\r");
    return 0;
}
