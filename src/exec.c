/* exec.c - program storage, statement execution, direct mode */
#include "basic.h"
#include "vessel.h"
#include <ctype.h>
#include <math.h>

void fn_clear(void);
void stmt_deffn(void);
void rnd_randomize(long n);
void rnd_reset(void);
void eval_reset_fn_depth(void);
void option_base_set(int b);
void *array_base(const char *name, int type, long *bytes);
void print_using_dummy(void);

u8 *txtptr;
u16 curline = 0xFFFF;
jmp_buf err_jmp;
int running = 0;
u8 *prog_mem;
unsigned prog_size;
int auto_mode = 0;
u16 auto_line = 10, auto_step = 10;
u16 err_line = 0xFFFF;
int err_code = 0;
int exit_after_run = 0;
unsigned def_seg = 0;
int trace_on = 0;
int ok_needed = 1;
unsigned p8_mem_top = 0;   /* CLEAR ,m : top of BASIC memory (0 = never set) */

#define PROG_MAX 0xFF00u
static u8 *cur_lp;         /* current line header (program) */
static u8 *stmt_start;     /* start of current statement */
static u8 *stmt_lp;
static jmp_buf run_jmp;
static int run_jmp_valid = 0;
static int prog_changed = 0;

/* ON ERROR */
static u16 onerr_line = 0;
static int in_handler = 0;
static u8 *resume_txt, *resume_lp;
static u16 resume_line;

/* CONT */
static u8 *cont_txt, *cont_lp;
static u16 cont_line;
static int cont_ok = 0;

/* DATA pointer */
static u8 *data_lp = 0, *data_p = 0;

/* direct mode buffer */
static u8 direct_buf[300];

/* ----------- control stack ----------- */
#define FR_FOR 1
#define FR_GOSUB 2
#define FR_WHILE 3
typedef struct {
    u8 kind;
    u8 vtype;
    u8 *txt;
    u8 *lp;
    u16 line;
    void *var;
    double limit, step;
    float flim, fstep;
    int ilim, istep;
    int trapret;    /* gosub from key trap */
} FRAME;
#define MAXFRAME 200
static FRAME stk[MAXFRAME];
static int sp = 0;

/* key traps */
static u16 keytrap_line[10];
static u8 keytrap_state[10];     /* 0 off 1 on 2 stop */
volatile u8 keytrap_pending[10];
volatile u8 keytrap_any = 0;
static int in_keytrap = 0;

/* ---------------- token helpers ---------------- */
u8 peekc(void)
{
    while (*txtptr == ' ') txtptr++;
    return *txtptr;
}

u8 getc_tok(void)
{
    u8 c = peekc();
    if (c) txtptr++;
    return c;
}

void syntax(void) { berror(E_SYNTAX); }

void expect(u8 c)
{
    if (peekc() != c) syntax();
    txtptr++;
}

int accept(u8 c)
{
    if (peekc() == c) { txtptr++; return 1; }
    return 0;
}

int accept_func(u8 f)
{
    if (peekc() == TK_FUNC && txtptr[1] == f) { txtptr += 2; return 1; }
    return 0;
}

int at_stmt_end(void)
{
    u8 c = peekc();
    return c == 0 || c == ':' || c == T_ELSE;
}

static void end_of_stmt(void)
{
    if (!at_stmt_end()) syntax();
}

/* skip one token at p (returns pointer after it) */
static u8 *skip_token(u8 *p)
{
    u8 c = *p;
    if (c == TK_FUNC) return p + 2;
    if (c == TK_INT || c == TK_LINENUM || c == TK_HEX || c == TK_OCT) return p + 3;
    if (c == TK_SNG) return p + 5;
    if (c == TK_DBL) return p + 9;
    if (c == '"') {
        p++;
        while (*p && *p != '"') p++;
        if (*p) p++;
        return p;
    }
    if (c == T_REM) { while (*p) p++; return p; }
    if (c == T_DATA) {
        int q = 0; p++;
        while (*p && (q || *p != ':')) { if (*p == '"') q = !q; p++; }
        return p;
    }
    return p + 1;
}

/* skip to end of current statement (':' or 0, optionally ELSE) */
static u8 *skip_stmt(u8 *p, int stop_else)
{
    while (*p && *p != ':' && !(stop_else && *p == T_ELSE)) p = skip_token(p);
    return p;
}

/* ---------------- program storage ---------------- */
u8 *prog_first(void) { return prog_mem; }
u16 line_num(u8 *l) { return (u16)(l[2] | (l[3] << 8)); }
u8 *line_next(u8 *l) { return l + (l[0] | (l[1] << 8)); }
static unsigned line_len(u8 *l) { return l[0] | (l[1] << 8); }

static u8 *prog_end(void)
{
    u8 *l = prog_mem;
    while (line_len(l)) l = line_next(l);
    return l;
}

unsigned prog_free(void)
{
    unsigned used = (unsigned)(prog_end() - prog_mem) + 2;
    return PROG_MAX - used;
}

/* exact: return line with num, or NULL. else: first line >= num (may be end) */
u8 *find_line(u16 num, int exact)
{
    u8 *l = prog_mem;
    while (line_len(l) && line_num(l) < num) l = line_next(l);
    if (exact) return (line_len(l) && line_num(l) == num) ? l : 0;
    return l;
}

void var_cache_clear(void);
static void invalidate(void)
{
    var_cache_clear();
    sp = 0;
    cont_ok = 0;
    fn_clear();
    data_lp = 0; data_p = 0;
    prog_changed = 1;
}

void prog_delete_range(u16 a, u16 b)
{
    u8 *s = find_line(a, 0), *e = s, *end;
    while (line_len(e) && line_num(e) <= b) e = line_next(e);
    if (e == s) return;
    end = prog_end() + 2;
    memmove(s, e, (size_t)(end - e));
    invalidate();
}

void prog_insert(u16 num, u8 *tok, int len)
{
    u8 *l = find_line(num, 0), *end;
    unsigned need = (unsigned)len + 4;
    if (line_len(l) && line_num(l) == num) prog_delete_range(num, num);
    l = find_line(num, 0);
    end = prog_end() + 2;
    if ((unsigned)(end - prog_mem) + need > PROG_MAX) berror(E_OUT_OF_MEM);
    memmove(l + need, l, (size_t)(end - l));
    l[0] = (u8)(need & 0xFF); l[1] = (u8)(need >> 8);
    l[2] = (u8)(num & 0xFF); l[3] = (u8)(num >> 8);
    memcpy(l + 4, tok, len);
    invalidate();
}

void prog_new(void)
{
    prog_mem[0] = prog_mem[1] = 0;
    invalidate();
    var_clear_all();
    file_close_all();
    onerr_line = 0;
    trace_on = 0;
}

/* ---------------- errors ---------------- */
static const struct { int code; const char *msg; } errmsg[] = {
 {1, "NEXT\x82\xC9\x91\xCE\x89\x9E\x82\xB7\x82\xE9" "FOR\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* NEXTに対応するFORがありません */
 {2, "\x95\xB6\x96@\x82\xAA\x90\xB3\x82\xB5\x82\xAD\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* 文法が正しくありません */
 {3, "GOSUB\x82\xCC\x82\xC8\x82\xA2RETURN\x82\xC5\x82\xB7"},  /* GOSUBのないRETURNです */
 {4, "DATA\x82\xAA\x91\xAB\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* DATAが足りません */
 {5, "\x8A\xD6\x90\x94\x81" "E\x96\xBD\x97\xDF\x82\xCC\x8Eg\x82\xA2\x95\xFB\x82\xAA\x90\xB3\x82\xB5\x82\xAD\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* 関数・命令の使い方が正しくありません */
 {6, "\x90\x94\x92l\x82\xAA\x94\xCD\x88\xCD\x82\xF0\x92\xB4\x82\xA6\x82\xDC\x82\xB5\x82\xBD"},  /* 数値が範囲を超えました */
 {7, "\x83\x81\x83\x82\x83\x8A\x82\xAA\x91\xAB\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* メモリが足りません */
 {8, "\x82\xBB\x82\xCC\x8Ds\x94\xD4\x8D\x86\x82\xCD\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* その行番号はありません */
 {9, "\x94z\x97\xF1\x82\xCC\x93Y\x8E\x9A\x82\xAA\x94\xCD\x88\xCD\x8AO\x82\xC5\x82\xB7"},  /* 配列の添字が範囲外です */
 {10, "\x94z\x97\xF1\x82\xAA\x93\xF1\x8F" "d\x82\xC9\x92\xE8\x8B`\x82\xB3\x82\xEA\x82\xC4\x82\xA2\x82\xDC\x82\xB7"},  /* 配列が二重に定義されています */
 {11, "0\x82\xC5\x8A\x84\x82\xE8\x82\xDC\x82\xB5\x82\xBD"},  /* 0で割りました */
 {12, "\x83_\x83" "C\x83\x8C\x83N\x83g\x83\x82\x81[\x83h\x82\xC5\x82\xCD\x8Eg\x82\xA6\x82\xDC\x82\xB9\x82\xF1"},  /* ダイレクトモードでは使えません */
 {13, "\x8C^\x82\xAA\x88\xEA\x92v\x82\xB5\x82\xDC\x82\xB9\x82\xF1"},  /* 型が一致しません */
 {14, "\x95\xB6\x8E\x9A\x97\xF1\x97\xCC\x88\xE6\x82\xAA\x91\xAB\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* 文字列領域が足りません */
 {15, "\x95\xB6\x8E\x9A\x97\xF1\x82\xAA\x92\xB7\x82\xB7\x82\xAC\x82\xDC\x82\xB7"},  /* 文字列が長すぎます */
 {16, "\x95\xB6\x8E\x9A\x97\xF1\x8E\xAE\x82\xAA\x95\xA1\x8EG\x82\xB7\x82\xAC\x82\xDC\x82\xB7"},  /* 文字列式が複雑すぎます */
 {17, "\x91\xB1\x8Ds\x82\xC5\x82\xAB\x82\xDC\x82\xB9\x82\xF1"},  /* 続行できません */
 {18, "\x92\xE8\x8B`\x82\xB3\x82\xEA\x82\xC4\x82\xA2\x82\xC8\x82\xA2\x83\x86\x81[\x83U\x81[\x8A\xD6\x90\x94\x82\xC5\x82\xB7"},  /* 定義されていないユーザー関数です */
 {19, "\x83G\x83\x89\x81[\x8F\x88\x97\x9D\x83\x8B\x81[\x83`\x83\x93\x82\xC9RESUME\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* エラー処理ルーチンにRESUMEがありません */
 {20, "\x83G\x83\x89\x81[\x8F\x88\x97\x9D\x92\x86\x82\xC5\x82\xCD\x82\xC8\x82\xA2\x82\xCC\x82\xC9RESUME\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB7"},  /* エラー処理中ではないのにRESUMEがあります */
 {22, "\x8E\xAE\x82\xCC\x92l\x82\xAA\x91\xAB\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* 式の値が足りません */
 {23, "1\x8Ds\x82\xAA\x92\xB7\x82\xB7\x82\xAC\x82\xDC\x82\xB7"},  /* 1行が長すぎます */
 {26, "FOR\x82\xC9\x91\xCE\x89\x9E\x82\xB7\x82\xE9NEXT\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* FORに対応するNEXTがありません */
 {29, "WHILE\x82\xC9\x91\xCE\x89\x9E\x82\xB7\x82\xE9WEND\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* WHILEに対応するWENDがありません */
 {30, "WEND\x82\xC9\x91\xCE\x89\x9E\x82\xB7\x82\xE9WHILE\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* WENDに対応するWHILEがありません */
 {50, "FIELD\x82\xCC\x92\xB7\x82\xB3\x82\xF0\x92\xB4\x82\xA6\x82\xDC\x82\xB5\x82\xBD"},  /* FIELDの長さを超えました */
 {51, "\x93\xE0\x95\x94\x83G\x83\x89\x81[\x82\xC5\x82\xB7"},  /* 内部エラーです */
 {52, "\x83t\x83@\x83" "C\x83\x8B\x94\xD4\x8D\x86\x82\xAA\x90\xB3\x82\xB5\x82\xAD\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* ファイル番号が正しくありません */
 {53, "\x83t\x83@\x83" "C\x83\x8B\x82\xAA\x8C\xA9\x82\xC2\x82\xA9\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* ファイルが見つかりません */
 {54, "\x83t\x83@\x83" "C\x83\x8B\x82\xCC\x83\x82\x81[\x83h\x82\xAA\x90\xB3\x82\xB5\x82\xAD\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* ファイルのモードが正しくありません */
 {55, "\x83t\x83@\x83" "C\x83\x8B\x82\xCD\x82\xB7\x82\xC5\x82\xC9\x8AJ\x82\xA2\x82\xC4\x82\xA2\x82\xDC\x82\xB7"},  /* ファイルはすでに開いています */
 {57, "\x93\xFC\x8Fo\x97\xCD\x83G\x83\x89\x81[\x82\xC5\x82\xB7"},  /* 入出力エラーです */
 {58, "\x93\xAF\x82\xB6\x96\xBC\x91O\x82\xCC\x83t\x83@\x83" "C\x83\x8B\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB7"},  /* 同じ名前のファイルがあります */
 {61, "\x83" "f\x83" "B\x83X\x83N\x82\xAA\x82\xA2\x82\xC1\x82\xCF\x82\xA2\x82\xC5\x82\xB7"},  /* ディスクがいっぱいです */
 {62, "\x83t\x83@\x83" "C\x83\x8B\x82\xCC\x8FI\x82\xED\x82\xE8\x82\xF0\x89z\x82\xA6\x82\xC4\x93\xC7\x82\xDD\x82\xDC\x82\xB5\x82\xBD"},  /* ファイルの終わりを越えて読みました */
 {63, "\x83\x8C\x83R\x81[\x83h\x94\xD4\x8D\x86\x82\xAA\x90\xB3\x82\xB5\x82\xAD\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* レコード番号が正しくありません */
 {64, "\x83t\x83@\x83" "C\x83\x8B\x96\xBC\x82\xAA\x90\xB3\x82\xB5\x82\xAD\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1"},  /* ファイル名が正しくありません */
 {66, "\x83t\x83@\x83" "C\x83\x8B\x92\x86\x82\xC9\x8Ds\x94\xD4\x8D\x86\x82\xCC\x82\xC8\x82\xA2\x8Ds\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB7"},  /* ファイル中に行番号のない行があります */
 {67, "\x8AJ\x82\xA2\x82\xC4\x82\xA2\x82\xE9\x83t\x83@\x83" "C\x83\x8B\x82\xAA\x91\xBD\x82\xB7\x82\xAC\x82\xDC\x82\xB7"},  /* 開いているファイルが多すぎます */
 {74, "\x95\xCA\x82\xCC\x83h\x83\x89\x83" "C\x83u\x82\xD6\x82\xCD\x96\xBC\x91O\x82\xF0\x95\xCF\x8DX\x82\xC5\x82\xAB\x82\xDC\x82\xB9\x82\xF1"},  /* 別のドライブへは名前を変更できません */
 {0,0}
};

static const char *err_text(int code)
{
    int i;
    for (i = 0; errmsg[i].msg; i++) if (errmsg[i].code == code) return errmsg[i].msg;
    return "\x83G\x83\x89\x81[\x82\xAA\x94\xAD\x90\xB6\x82\xB5\x82\xDC\x82\xB5\x82\xBD";  /* エラーが発生しました */
}

static jmp_buf top_jmp;

void berror(int code)
{
    err_code = code;
    err_line = curline;
    if (running && run_jmp_valid && onerr_line && !in_handler) {
        resume_txt = stmt_start; resume_lp = stmt_lp; resume_line = curline;
        longjmp(run_jmp, 2);
    }
    /* report */
    if (running) {
        cont_ok = 0;
    }
    out_fp = 0;
    if (cur_x != 0) con_newline();
    con_puts(err_text(code));
    if (curline != 0xFFFF) {
        char b[24]; sprintf(b, "\x81i%u\x8Ds\x81j", curline); con_puts(b);  /* （%u行） */
    }
    con_newline();
    running = 0;
    in_handler = 0;
    auto_mode = 0;
    longjmp(top_jmp, 1);
}

/* ---------------- line/data helpers ---------------- */
/* ---------------- labels (*NAME) ---------------- */
static int label_char(u8 c) { return isalnum(c) || c == '.'; }

/* find the line whose first statement is "*name"; returns line number */
static u16 find_label(const u8 *name, int len)
{
    u8 *l = prog_mem;
    while (line_len(l)) {
        u8 *p = l + 4;
        while (*p == ' ') p++;
        if (*p == '*') {
            int j;
            p++;
            for (j = 0; j < len && p[j] == name[j]; j++) {}
            if (j == len && !label_char(p[j])) return line_num(l);
        }
        l = line_next(l);
    }
    berror(E_UNDEF_LINE);
    return 0;
}

static u16 get_linenum(void)
{
    u16 n;
    if (peekc() == '*' && isalpha(txtptr[1])) {
        u8 *nm = ++txtptr;
        while (label_char(*txtptr)) txtptr++;
        return find_label(nm, (int)(txtptr - nm));
    }
    if (peekc() == TK_LINENUM || peekc() == TK_INT) {
        txtptr++;
        n = (u16)(txtptr[0] | (txtptr[1] << 8));
        txtptr += 2;
        return n;
    }
    syntax();
    return 0;
}

static int has_linenum(void)
{
    u8 c = peekc();
    return c == TK_LINENUM || c == TK_INT || (c == '*' && isalpha(txtptr[1]));
}

static void goto_line(u16 n)
{
    u8 *l = find_line(n, 1);
    if (!l) berror(E_UNDEF_LINE);
    cur_lp = l;
    curline = n;
    txtptr = l + 4;
    if (!running) running = 1;
}

/* ---------------- RUN control ---------------- */
static void clear_runtime(void)
{
    var_clear_all();
    fn_clear();
    sp = 0;
    data_lp = 0; data_p = 0;
    onerr_line = 0; in_handler = 0;
    cont_ok = 0;
    memset(keytrap_line, 0, sizeof(keytrap_line));
    memset(keytrap_state, 0, sizeof(keytrap_state));
    memset((void *)keytrap_pending, 0, sizeof(keytrap_pending));
    in_keytrap = 0;
    def_seg = hal_dataseg();
}

static void exec_stmt(void);

static void do_end(int is_stop)
{
    if (is_stop || running) {
        cont_txt = txtptr; cont_lp = cur_lp; cont_line = curline; cont_ok = 1;
    }
    if (!is_stop) {
        file_close_all();
        if (exit_after_run) { hal_term(); exit(0); }
    }
    running = 0;
    if (is_stop) {
        char b[40];
        if (cur_x) con_newline();
        sprintf(b, "\x92\x86\x92" "f\x82\xB5\x82\xDC\x82\xB5\x82\xBD\x81i%u\x8Ds\x81j", curline);  /* 中断しました（%u行） */
        con_puts(b); con_newline();
    }
    longjmp(top_jmp, 2);
}

void key_traps_check(void);

/* main execution loop: txtptr set; executes until end/direct line end */
static void exec_loop(void)
{
    int r;
    run_jmp_valid = 1;
    r = setjmp(run_jmp);
    if (r == 2) {
        /* error trapped */
        in_handler = 1;
        goto_line(onerr_line);
    }
    for (;;) {
        u8 c;
        vessel_statement(); /* the machine advances by one statement's
                             * modelled cost while BASIC runs (PMD
                             * playback included) */
        hal_poll();         /* embedded hosts may bail out here */
        str_temp_reset();
        if (running && hal_breakflag()) {
            if (exit_after_run) { /* stop running file: fall to editor */ exit_after_run = 0; }
            do_end(1);
        }
        if (running) key_traps_check();
        c = peekc();
        if (c == ':') { txtptr++; continue; }
        if (c == 0) {
            if (curline == 0xFFFF || !running) { run_jmp_valid = 0; return; }
            cur_lp = line_next(cur_lp);
            if (!line_len(cur_lp)) {
                running = 0; run_jmp_valid = 0;
                if (sp > 0) { int k; for (k = sp - 1; k >= 0; k--) if (stk[k].kind == FR_WHILE) { /* no WEND */ } }
                file_close_all();
                if (exit_after_run) { hal_term(); exit(0); }
                curline = 0xFFFF;
                return;
            }
            curline = line_num(cur_lp);
            txtptr = cur_lp + 4;
            if (trace_on) { char b[12]; sprintf(b, "[%u]", curline); con_puts(b); }
            continue;
        }
        stmt_start = txtptr; stmt_lp = cur_lp;
        exec_stmt();
    }
}

void run_program(u8 *start)
{
    if (!line_len(start)) { running = 0; return; }
    cur_lp = start;
    curline = line_num(start);
    txtptr = start + 4;
    running = 1;
    prog_changed = 0;
    exec_loop();
}

/* ---------------- FOR/NEXT ---------------- */
static void stmt_next(void);
static void stmt_for(void)
{
    int t, k;
    void *vp;
    VAL v, lim, st;
    FRAME *f;
    peekc();
    vp = var_ptr_for(0, &t, 1);
    if (t == T_STR) berror(E_TYPE);
    expect('=');
    eval_num(&v); var_set(vp, t, &v);
    expect(T_TO);
    eval_num(&lim);
    if (accept(T_STEP)) eval_num(&st); else { st.type = T_INT; st.u.i = 1; }
    /* remove existing frame for same var */
    for (k = sp - 1; k >= 0; k--) {
        if (stk[k].kind == FR_GOSUB) break;
        if (stk[k].kind == FR_FOR && stk[k].var == vp) { sp = k; break; }
    }
    if (sp >= MAXFRAME) berror(E_OUT_OF_MEM);
    f = &stk[sp];
    f->kind = FR_FOR; f->vtype = (u8)t; f->var = vp;
    if (t == T_INT) { f->ilim = val_to_int(&lim); f->istep = val_to_int(&st); f->limit = f->ilim; f->step = f->istep; }
    else if (t == T_SNG) { f->limit = (float)val_to_dbl(&lim); f->step = (float)val_to_dbl(&st); f->flim = (float)f->limit; f->fstep = (float)f->step; }
    else { f->limit = val_to_dbl(&lim); f->step = val_to_dbl(&st); }
    end_of_stmt();
    f->txt = txtptr; f->lp = cur_lp; f->line = curline;
    {
        /* initial test: skip loop if already past limit */
        VAL cv; double x;
        var_get(vp, t, &cv); x = val_to_dbl(&cv);
        if ((f->step >= 0 && x > f->limit) || (f->step < 0 && x < f->limit)) {
            /* find matching NEXT */
            int depth = 1;
            u8 *p = txtptr; u8 *lp = cur_lp; u16 ln = curline;
            for (;;) {
                if (*p == 0) {
                    if (ln == 0xFFFF) berror(E_FOR_WO_NEXT);
                    lp = line_next(lp);
                    if (!line_len(lp)) berror(E_FOR_WO_NEXT);
                    ln = line_num(lp); p = lp + 4; continue;
                }
                if (*p == T_FOR) { depth++; p++; continue; }
                if (*p == T_NEXT) {
                    u8 *q = p + 1;
                    while (*q == ' ') q++;
                    if (*q == 0 || *q == ':' || *q == T_ELSE) { depth--; p = q; }
                    else {
                        for (;;) {
                            depth--;
                            while (*q && *q != ',' && *q != ':' && *q != T_ELSE) q = skip_token(q);
                            if (depth == 0 || *q != ',') break;
                            q++;
                        }
                        p = q;
                    }
                    if (depth <= 0) {
                        cur_lp = lp; curline = ln;
                        txtptr = p;
                        if (*txtptr == ',') { txtptr++; stmt_next(); }
                        return;
                    }
                    continue;
                }
                p = skip_token(p);
            }
        }
    }
    sp++;
}

static void stmt_next(void)
{
    for (;;) {
        void *vp = 0;
        int t, k;
        FRAME *f;
        VAL cv;
        double x;
        if (!at_stmt_end()) { peekc(); vp = var_ptr_for(0, &t, 1); }
        for (k = sp - 1; k >= 0; k--) {
            if (stk[k].kind != FR_FOR) { if (stk[k].kind == FR_GOSUB) k = -1; break; }
            if (!vp || stk[k].var == vp) break;
        }
        if (k < 0 || stk[k].kind != FR_FOR) berror(E_NEXT_WO_FOR);
        sp = k + 1;
        f = &stk[k];
        var_get(f->var, f->vtype, &cv);
        if (f->vtype == T_INT) {
            long nv = (long)*(i16 *)f->var + f->istep;
            if (nv > 32767L || nv < -32768L) berror(E_OVERFLOW);
            *(i16 *)f->var = (i16)nv;
            if ((f->istep >= 0 && (int)nv <= f->ilim) || (f->istep < 0 && (int)nv >= f->ilim)) {
                txtptr = f->txt; cur_lp = f->lp; curline = f->line;
                return;
            }
            sp = k;
            if (!accept(',')) break;
            continue;
        } else if (f->vtype == T_SNG) {
            float nf = cv.u.f + f->fstep;
            *(float *)f->var = nf;
            if ((f->fstep >= 0 && nf <= f->flim) || (f->fstep < 0 && nf >= f->flim)) {
                txtptr = f->txt; cur_lp = f->lp; curline = f->line;
                return;
            }
            sp = k;
            if (!accept(',')) break;
            continue;
        }
        else { cv.u.d = cv.u.d + f->step; x = cv.u.d; }
        if (f->vtype == T_INT) *(i16 *)f->var = cv.u.i; else *(double *)f->var = cv.u.d;
        if ((f->step >= 0 && x <= f->limit) || (f->step < 0 && x >= f->limit)) {
            txtptr = f->txt; cur_lp = f->lp; curline = f->line;
            return;
        }
        sp = k;
        if (!accept(',')) break;
    }
    end_of_stmt();
}

/* ---------------- WHILE/WEND ---------------- */
static void stmt_while(void)
{
    u8 *wtxt = txtptr;     /* right after WHILE token */
    VAL v;
    eval_num(&v);
    if (val_to_dbl(&v) != 0) {
        if (sp >= MAXFRAME) berror(E_OUT_OF_MEM);
        stk[sp].kind = FR_WHILE; stk[sp].txt = wtxt; stk[sp].lp = cur_lp; stk[sp].line = curline;
        sp++;
        return;
    }
    /* skip to matching WEND */
    {
        int depth = 1;
        u8 *p = txtptr, *lp = cur_lp; u16 ln = curline;
        for (;;) {
            if (*p == 0) {
                if (ln == 0xFFFF) berror(E_WHILE_WO_WEND);
                lp = line_next(lp);
                if (!line_len(lp)) berror(E_WHILE_WO_WEND);
                ln = line_num(lp); p = lp + 4; continue;
            }
            if (*p == T_WHILE) depth++;
            else if (*p == T_WEND) { if (--depth == 0) { txtptr = p + 1; cur_lp = lp; curline = ln; return; } }
            p = skip_token(p);
        }
    }
}

static void stmt_wend(void)
{
    FRAME *f;
    if (sp == 0 || stk[sp - 1].kind != FR_WHILE) berror(E_WEND_WO_WHILE);
    f = &stk[--sp];
    txtptr = f->txt; cur_lp = f->lp; curline = f->line;
    stmt_while();
}

/* ---------------- GOSUB/RETURN ---------------- */
static void push_gosub(int trap)
{
    if (sp >= MAXFRAME) berror(E_OUT_OF_MEM);
    stk[sp].kind = FR_GOSUB; stk[sp].txt = txtptr; stk[sp].lp = cur_lp; stk[sp].line = curline;
    stk[sp].trapret = trap;
    sp++;
}

static void stmt_return(void)
{
    int k;
    for (k = sp - 1; k >= 0; k--) if (stk[k].kind == FR_GOSUB) break;
    if (k < 0) berror(E_RET_WO_GOSUB);
    sp = k;
    if (stk[k].trapret) in_keytrap = 0;
    if (has_linenum()) { u16 n = get_linenum(); goto_line(n); return; }
    txtptr = stk[k].txt; cur_lp = stk[k].lp; curline = stk[k].line;
    if (curline != 0xFFFF && !running) running = 1;
}

/* ---------------- IF ---------------- */
static void exec_after_then(void)
{
    if (has_linenum()) { goto_line(get_linenum()); return; }
    /* statements follow; continue execution from here */
}

static void stmt_if(void)
{
    VAL v;
    int cond;
    eval(&v);
    if (v.type == T_STR) cond = v.u.s.len != 0; else cond = val_to_dbl(&v) != 0;
    if (peekc() == T_THEN) txtptr++;
    else if (peekc() == T_GOTO) {
        txtptr++;
        if (cond) { goto_line(get_linenum()); return; }
    } else syntax();
    if (cond) { exec_after_then(); return; }
    /* find matching ELSE */
    {
        int depth = 0;
        u8 *p = txtptr;
        while (*p) {
            if (*p == T_IF) depth++;
            else if (*p == T_ELSE) {
                if (depth == 0) {
                    txtptr = p + 1;
                    exec_after_then();
                    return;
                }
                depth--;
            }
            p = skip_token(p);
        }
        txtptr = p;
    }
}

/* ---------------- ON ---------------- */
static void stmt_on(void)
{
    int n, i, isgosub;
    if (peekc() == T_ERROR) {
        txtptr++;
        expect(T_GOTO);
        onerr_line = get_linenum();
        if (onerr_line && !find_line(onerr_line, 1)) berror(E_UNDEF_LINE);
        if (onerr_line == 0 && in_handler) {
            /* ON ERROR GOTO 0 inside handler: report the error */
            in_handler = 0;
            curline = err_line;
            berror(err_code);
        }
        return;
    }
    if (peekc() == T_KEY) {
        txtptr++;
        expect(T_GOSUB);
        for (i = 0; i < 10; i++) {
            if (has_linenum()) keytrap_line[i] = get_linenum();
            if (!accept(',')) break;
        }
        return;
    }
    n = eval_int();
    if (n < 0 || n > 255) berror(E_ILLEGAL_FN);
    if (accept(T_GOTO)) isgosub = 0;
    else if (accept(T_GOSUB)) isgosub = 1;
    else syntax();
    for (i = 1; ; i++) {
        u16 ln = get_linenum();
        if (i == n) {
            u8 *rest = skip_stmt(txtptr, 1);
            if (isgosub) { u8 *save = txtptr; txtptr = rest; push_gosub(0); txtptr = save; }
            goto_line(ln);
            return;
        }
        if (!accept(',')) break;
    }
    /* out of range: fall through */
}

/* ---------------- key traps ---------------- */
void key_trap_signal(int n)
{
    if (n >= 0 && n < 10 && keytrap_state[n] && keytrap_line[n]) { keytrap_pending[n] = 1; keytrap_any = 1; }
}

int key_trap_enabled(int n)
{
    return n >= 0 && n < 10 && keytrap_state[n] && keytrap_line[n];
}

void key_traps_check(void)
{
    int i;
    if (!keytrap_any || in_keytrap) return;
    keytrap_any = 0;
    for (i = 0; i < 10; i++) if (keytrap_pending[i]) keytrap_any = 1;
    for (i = 0; i < 10; i++) {
        if (keytrap_pending[i] && keytrap_state[i] == 1) {
            keytrap_pending[i] = 0;
            push_gosub(1);
            in_keytrap = 1;
            goto_line(keytrap_line[i]);
            return;
        }
    }
}

void key_stmt_trap(int n, int state)
{
    if (n < 1 || n > 10) berror(E_ILLEGAL_FN);
    keytrap_state[n - 1] = (u8)state;
    if (state == 0) keytrap_pending[n - 1] = 0;
}

/* ---------------- assignment ---------------- */
static void stmt_let(void)
{
    int t;
    void *vp;
    VAL v;
    peekc();
    vp = var_ptr_for(0, &t, 1);
    expect('=');
    eval(&v);
    if ((v.type == T_STR) != (t == T_STR)) berror(E_TYPE);
    var_set(vp, t, &v);
}

static void stmt_mid_assign(void)
{
    int t, n, m = 255;
    void *vp;
    VAL cur, rep;
    expect('(');
    peekc();
    vp = var_ptr_for(0, &t, 1);
    if (t != T_STR) berror(E_TYPE);
    expect(','); n = eval_int();
    if (accept(',')) m = eval_int();
    expect(')'); expect('=');
    eval_str(&rep);
    var_get(vp, t, &cur);
    if (n < 1 || n > 255 || m < 0) berror(E_ILLEGAL_FN);
    if (n <= cur.u.s.len) {
        char *p = str_temp(cur.u.s.len);
        unsigned cnt = rep.u.s.len;
        memcpy(p, cur.u.s.p, cur.u.s.len);
        if (cnt > (unsigned)m) cnt = m;
        if (cnt > cur.u.s.len - (n - 1)) cnt = cur.u.s.len - (n - 1);
        memcpy(p + n - 1, rep.u.s.p, cnt);
        cur.u.s.p = p;
        var_set(vp, t, &cur);
    }
}

/* ---------------- DIM / ERASE / SWAP ---------------- */
static void stmt_dim(void)
{
    do {
        char name[41]; int t, d[8], n = 0;
        peekc();
        parse_varname(name, &t);
        expect('(');
        do { if (n >= 8) syntax(); d[n++] = eval_int(); } while (accept(','));
        expect(')');
        dim_array(name, t, n, d);
    } while (accept(','));
}

static void stmt_erase(void)
{
    do {
        char name[41]; int t;
        peekc();
        parse_varname(name, &t);
        var_erase(name, t);
    } while (accept(','));
}

static void stmt_swap(void)
{
    int t1, t2;
    void *a, *b;
    u8 tmp[16];
    static const int sz[9] = {0,0,2,4,4,0,0,0,8};
    peekc(); a = var_ptr_for(0, &t1, 1);
    expect(',');
    peekc(); b = var_ptr_for(0, &t2, 1);
    if (t1 != t2) berror(E_TYPE);
    {
        int n = (t1 == T_STR) ? (int)(sizeof(u16) + sizeof(char *)) : sz[t1];
        if (t1 == T_STR) n = sizeof(struct { u16 l; char *p; });
        memcpy(tmp, a, n); memcpy(a, b, n); memcpy(b, tmp, n);
    }
}

/* ---------------- READ / DATA / RESTORE ---------------- */
static int data_find_next(void)
{
    /* position data_p at start of next DATA item list. */
    if (!data_lp) { data_lp = prog_mem; data_p = data_lp + 4; if (!line_len(data_lp)) return 0; }
    for (;;) {
        if (*data_p == 0) {
            data_lp = line_next(data_lp);
            if (!line_len(data_lp)) { data_p = data_lp; return 0; }
            data_p = data_lp + 4;
            continue;
        }
        if (*data_p == T_DATA) { data_p++; return 1; }
        data_p = skip_token(data_p);
    }
}

static int data_in_item = 0;

static void read_item(char *buf, int max, int *quoted)
{
    int n = 0;
    *quoted = 0;
    if (!data_in_item || !data_lp) {
        if (!data_find_next()) berror(E_OUT_OF_DATA);
        data_in_item = 1;
    }
    while (*data_p == ' ') data_p++;
    if (*data_p == '"') {
        *quoted = 1; data_p++;
        while (*data_p && *data_p != '"') { if (n < max - 1) buf[n++] = *data_p; data_p++; }
        if (*data_p == '"') data_p++;
        while (*data_p == ' ') data_p++;
    } else {
        while (*data_p && *data_p != ',' && *data_p != ':') { if (n < max - 1) buf[n++] = *data_p; data_p++; }
        while (n > 0 && buf[n - 1] == ' ') n--;
    }
    buf[n] = 0;
    if (*data_p == ',') data_p++;
    else data_in_item = 0;   /* end of this DATA statement */
}

static void stmt_read(void)
{
    do {
        int t, q;
        void *vp;
        char buf[260];
        VAL v;
        u16 save = curline;
        peekc();
        vp = var_ptr_for(0, &t, 1);
        read_item(buf, sizeof(buf), &q);
        if (t == T_STR) {
            v.type = T_STR; v.u.s.len = (u16)strlen(buf); v.u.s.p = buf;
            var_set(vp, t, &v);
        } else {
            char *p = buf; int neg = 0, used = 0;
            while (*p == ' ') p++;
            if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
            if (*p == 0) { v.type = T_INT; v.u.i = 0; }
            else if (*p == '&') {
                VAL hv; u8 tk[32]; u8 *sv = txtptr;
                tokenize(p, tk, sizeof(tk));
                txtptr = tk; eval_num(&hv); txtptr = sv; v = hv;
            } else {
                if (!isdigit((u8)*p) && *p != '.') { curline = line_num(data_lp); (void)save; berror(E_SYNTAX); }
                str_to_num(p, &v, &used);
                p += used; while (*p == ' ') p++;
                if (*p) { curline = line_num(data_lp); berror(E_SYNTAX); }
            }
            if (neg) {
                if (v.type == T_INT) { if (v.u.i == -32768) { v.type = T_SNG; v.u.f = 32768.0f; } else v.u.i = -v.u.i; }
                else if (v.type == T_SNG) v.u.f = -v.u.f; else v.u.d = -v.u.d;
            }
            var_set(vp, t, &v);
        }
    } while (accept(','));
}

static void stmt_restore(void)
{
    data_in_item = 0;
    if (has_linenum()) {
        u16 n = get_linenum();
        u8 *l = find_line(n, 0);
        if (!line_len(l)) { data_lp = l; data_p = l; return; }
        data_lp = l; data_p = l + 4;
        return;
    }
    data_lp = 0; data_p = 0;
}

/* ---------------- INPUT ---------------- */
static int parse_input_item(char **pp, int type, VAL *v, char *sbuf)
{
    char *p = *pp;
    int n = 0;
    while (*p == ' ') p++;
    if (type == T_STR) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (n < 255) sbuf[n++] = *p; p++; }
            if (*p == '"') p++;
            while (*p == ' ') p++;
        } else {
            while (*p && *p != ',') { if (n < 255) sbuf[n++] = *p; p++; }
            while (n > 0 && sbuf[n - 1] == ' ') n--;
        }
        sbuf[n] = 0;
        v->type = T_STR; v->u.s.len = (u16)n; v->u.s.p = sbuf;
    } else {
        int neg = 0, used = 0;
        if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
        while (*p == ' ') p++;
        if (*p == ',' || *p == 0) { v->type = T_INT; v->u.i = 0; }
        else if (*p == '&') {
            long lv = 0; int hex = (p[1] == 'H' || p[1] == 'h');
            p += (p[1] == 'H' || p[1] == 'h' || p[1] == 'O' || p[1] == 'o') ? 2 : 1;
            while (*p && *p != ',' && *p != ' ') {
                int c = toupper((u8)*p);
                if (hex && isxdigit(c)) lv = lv * 16 + (isdigit(c) ? c - '0' : c - 'A' + 10);
                else if (!hex && c >= '0' && c <= '7') lv = lv * 8 + c - '0';
                else return 0;
                p++;
            }
            v->type = T_INT; v->u.i = (i16)(u16)lv;
        } else {
            if (!isdigit((u8)*p) && *p != '.') return 0;
            str_to_num(p, v, &used);
            p += used;
        }
        while (*p == ' ') p++;
        if (*p && *p != ',') return 0;
        if (neg) {
            if (v->type == T_INT) { if (v->u.i == -32768) { v->type = T_SNG; v->u.f = 32768.0f; } else v->u.i = -v->u.i; }
            else if (v->type == T_SNG) v->u.f = -v->u.f; else v->u.d = -v->u.d;
        }
    }
    *pp = p;
    return 1;
}

static void stmt_input(int line_input)
{
    VAL pv;
    int has_prompt = 0, qmark = 1;
    u8 *vars;
    char buf[300];
    if (peekc() == '#') {
        /* INPUT# */
        int fno; FILE *fp;
        txtptr++;
        fno = eval_int(); expect(',');
        fp = file_get(fno, 'I');
        do {
            int t; void *vp; VAL v; char sbuf[260];
            peekc();
            vp = var_ptr_for(0, &t, 1);
            if (line_input) {
                int n = 0, c;
                c = fgetc(fp);
                if (c == EOF) berror(E_INPUT_PAST);
                while (c != EOF && c != '\r' && c != '\n' && c != 0x1A) { if (n < 255) sbuf[n++] = (char)c; c = fgetc(fp); }
                if (c == '\r') { c = fgetc(fp); if (c != '\n' && c != EOF) ungetc(c, fp); }
                if (c == 0x1A) ungetc(c, fp);
                v.type = T_STR; v.u.s.len = (u16)n; v.u.s.p = sbuf;
                if (t != T_STR) berror(E_TYPE);
                var_set(vp, t, &v);
            } else {
                int c, n = 0, q = 0;
                char *pp;
                /* skip leading whitespace/newlines */
                do { c = fgetc(fp); } while (c == ' ' || c == '\r' || c == '\n' || c == '\t');
                if (c == EOF || c == 0x1A) berror(E_INPUT_PAST);
                if (c == '"' && t == T_STR) {
                    q = 1;
                    c = fgetc(fp);
                    while (c != EOF && c != '"') { if (n < 255) sbuf[n++] = (char)c; c = fgetc(fp); }
                    c = fgetc(fp);
                    while (c == ' ') c = fgetc(fp);
                    if (c != ',' && c != '\r' && c != '\n' && c != EOF) ungetc(c, fp);
                    if (c == '\r') { c = fgetc(fp); if (c != '\n' && c != EOF) ungetc(c, fp); }
                    sbuf[n] = 0;
                    v.type = T_STR; v.u.s.len = (u16)n; v.u.s.p = sbuf;
                } else {
                    while (c != EOF && c != ',' && c != '\r' && c != '\n' && c != 0x1A &&
                           !(t != T_STR && c == ' ' && n > 0)) {
                        if (n < 255) sbuf[n++] = (char)c;
                        c = fgetc(fp);
                    }
                    if (c == '\r') { c = fgetc(fp); if (c != '\n' && c != EOF) ungetc(c, fp); }
                    if (c == 0x1A) ungetc(c, fp);
                    while (n > 0 && sbuf[n - 1] == ' ') n--;
                    sbuf[n] = 0;
                    pp = sbuf;
                    if (t == T_STR) { v.type = T_STR; v.u.s.len = (u16)n; v.u.s.p = sbuf; }
                    else if (!parse_input_item(&pp, t, &v, buf)) { v.type = T_INT; v.u.i = 0; }
                }
                (void)q;
                var_set(vp, t, &v);
            }
        } while (accept(','));
        return;
    }
    if (accept(';')) { /* suppress CR - ignored */ }
    if (peekc() == '"') {
        eval(&pv);
        has_prompt = 1;
        if (accept(';')) qmark = 1;
        else if (accept(',')) qmark = 0;
        else syntax();
    }
    if (line_input) qmark = 0;
    vars = txtptr;
    for (;;) {
        char *p;
        int ok = 1;
        if (has_prompt) out_str(pv.u.s.p, pv.u.s.len);
        if (qmark) con_puts("? ");
        if (con_lineinput(buf, sizeof(buf), 1) < 0) {
            /* STOP pressed */
            do_end(1);
        }
        p = buf;
        txtptr = vars;
        if (line_input) {
            int t; void *vp; VAL v;
            peekc();
            vp = var_ptr_for(0, &t, 1);
            if (t != T_STR) berror(E_TYPE);
            v.type = T_STR; v.u.s.len = (u16)strlen(buf); v.u.s.p = buf;
            var_set(vp, t, &v);
            return;
        }
        /* first pass: validate */
        {
            int cnt = 0;
            char sb[260];
            do {
                int t; void *vp; VAL v;
                peekc();
                vp = var_ptr_for(0, &t, 1);
                (void)vp;
                if (cnt > 0) { while (*p == ' ') p++; if (*p != ',') { ok = 0; break; } p++; }
                if (!parse_input_item(&p, t, &v, sb)) { ok = 0; break; }
                if (t != T_STR) { VAL tv = v; if (tv.type != t) { /* overflow check */ if (t == T_INT) { double d = val_to_dbl(&tv); if (d < -32768.0 || d > 32767.0) { ok = 0; break; } } } }
                cnt++;
            } while (accept(','));
            while (*p == ' ') p++;
            if (ok && *p) ok = 0;
        }
        if (!ok) { con_puts("\x3F\x82\xE0\x82\xA4\x88\xEA\x93x\x93\xFC\x97\xCD\x82\xB5\x82\xC4\x82\xAD\x82\xBE\x82\xB3\x82\xA2");  /* ?もう一度入力してください */ con_newline(); continue; }
        /* second pass: assign */
        p = buf; txtptr = vars;
        {
            int cnt = 0; char sb[260];
            do {
                int t; void *vp; VAL v;
                peekc();
                vp = var_ptr_for(0, &t, 1);
                if (cnt > 0) { while (*p == ' ') p++; p++; }
                parse_input_item(&p, t, &v, sb);
                var_set(vp, t, &v);
                cnt++;
            } while (accept(','));
        }
        return;
    }
}

/* ---------------- LIST / RENUM / DELETE ---------------- */
static int parse_range(u16 *a, u16 *b)
{
    *a = 0; *b = 65529u;
    if (at_stmt_end()) return 0;
    if (accept('.')) { *a = *b = (curline == 0xFFFF ? err_line : curline); if (*a == 0xFFFF) *a = *b = 0; }
    else if (has_linenum()) *a = *b = get_linenum();
    if (accept('-')) {
        *b = 65529u;
        if (has_linenum()) *b = get_linenum();
        else if (accept('.')) *b = err_line;
    }
    return 1;
}

void list_program(u16 a, u16 b, FILE *fp)
{
    u8 *l = find_line(a, 0);
    char buf[512];
    while (line_len(l) && line_num(l) <= b) {
        int n = sprintf(buf, "%u ", line_num(l));
        detokenize_line(l + 4, buf + n, sizeof(buf) - n);
        if (fp) { fputs(buf, fp); fputs("\r\n", fp); }
        else {
            if (hal_breakflag()) break;
            con_puts(buf); con_newline();
        }
        l = line_next(l);
    }
}

static void stmt_list(void)
{
    u16 a, b;
    parse_range(&a, &b);
    if (accept(',')) {
        /* LIST ,"file" -> save ascii */
        VAL f; char fn[80];
        eval_str(&f);
        file_fname(f.u.s.p, f.u.s.len, fn, ".BAS");
        {
            FILE *fp = fopen(fn, "wb");
            if (!fp) berror(E_IO);
            list_program(a, b, fp);
            fclose(fp);
        }
        return;
    }
    list_program(a, b, 0);
}

static void stmt_delete(void)
{
    u16 a, b;
    if (!parse_range(&a, &b)) berror(E_ILLEGAL_FN);
    if (a == b && !find_line(a, 1)) berror(E_ILLEGAL_FN);
    prog_delete_range(a, b);
    if (running) { running = 0; longjmp(top_jmp, 2); }
}

static void stmt_renum(void)
{
    u16 nw = 10, old = 0, inc = 10;
    u8 *l;
    u16 cnt = 0;
    if (has_linenum()) nw = get_linenum();
    if (accept(',')) { if (has_linenum()) old = get_linenum(); if (accept(',')) inc = (u16)eval_int(); }
    if (inc == 0) berror(E_ILLEGAL_FN);
    /* check */
    l = prog_mem;
    {
        u16 prev = 0; int has_prev = 0;
        while (line_len(l) && line_num(l) < old) { prev = line_num(l); has_prev = 1; l = line_next(l); }
        if (has_prev && nw <= prev) berror(E_ILLEGAL_FN);
        {
            u8 *q = l; long n = nw;
            while (line_len(q)) { cnt++; if (n > 65529L) berror(E_ILLEGAL_FN); n += inc; q = line_next(q); }
        }
    }
    /* update references */
    {
        u8 *ln = prog_mem;
        while (line_len(ln)) {
            u8 *p = ln + 4;
            while (*p) {
                if (*p == TK_LINENUM) {
                    u16 ref = (u16)(p[1] | (p[2] << 8));
                    if (ref >= old && ref != 0) {
                        /* find index of ref */
                        u8 *q = l; long n = nw; int found = 0;
                        while (line_len(q)) { if (line_num(q) == ref) { found = 1; break; } n += inc; q = line_next(q); }
                        if (found) { p[1] = (u8)(n & 0xFF); p[2] = (u8)(n >> 8); }
                        else {
                            char b[64]; sprintf(b, "%u\x8Ds\x82\xAA\x8EQ\x8F\xC6\x82\xB7\x82\xE9%u\x8Ds\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82\xF1", line_num(ln), ref);  /* %u行が参照する%u行がありません */
                            con_puts(b); con_newline();
                        }
                    }
                }
                p = skip_token(p);
            }
            ln = line_next(ln);
        }
    }
    {
        long n = nw;
        while (line_len(l)) { l[2] = (u8)(n & 0xFF); l[3] = (u8)(n >> 8); n += inc; l = line_next(l); }
    }
    invalidate();
    (void)cnt;
}

/* ---------------- misc statements ---------------- */
static void stmt_clear(void)
{
    /* CLEAR [n[,m]] : only m (memory top) is recorded, for BLOAD's default address */
    if (!at_stmt_end()) { if (peekc() != ',') eval_long(); if (accept(',')) p8_mem_top = (unsigned)(eval_long() & 0xFFFF); if (accept(',')) eval_long(); }
    var_clear_all();
    fn_clear();
    file_close_all();
    sp = 0; data_lp = 0; data_p = 0;
}

static void stmt_run(void)
{
    if (peekc() == '"') {
        VAL f; char fn[80];
        int keepfiles = 0;
        eval_str(&f);
        if (accept(',')) { /* ,R : keep files */ if (toupper(peekc()) == 'R') { txtptr++; keepfiles = 1; } }
        file_fname(f.u.s.p, f.u.s.len, fn, ".BAS");
        if (!keepfiles) file_close_all();
        if (!prog_load(fn, 0)) berror(E_FILE_NF);
        clear_runtime();
        run_program(prog_mem);
        longjmp(top_jmp, 3);
    }
    {
        u8 *start = prog_mem;
        if (has_linenum()) { u16 n = get_linenum(); start = find_line(n, 1); if (!start) berror(E_UNDEF_LINE); }
        clear_runtime();
        file_close_all();
        run_program(start);
        longjmp(top_jmp, 3);
    }
}

static void stmt_cont(void)
{
    if (!cont_ok || prog_changed) berror(E_CANT_CONT);
    txtptr = cont_txt; cur_lp = cont_lp; curline = cont_line;
    cont_ok = 0;
    running = 1;
    exec_loop();
    longjmp(top_jmp, 3);
}

static void stmt_resume(void)
{
    if (!in_handler) berror(E_RESUME_WO_ERR);
    in_handler = 0;
    if (accept(T_NEXT)) {
        txtptr = resume_txt; cur_lp = resume_lp; curline = resume_line;
        txtptr = skip_stmt(txtptr, 0);
        return;
    }
    if (has_linenum()) {
        u16 n = get_linenum();
        if (n != 0) { goto_line(n); return; }
    }
    txtptr = resume_txt; cur_lp = resume_lp; curline = resume_line;
}

static void stmt_def(void)
{
    if (peekc() == T_FN) { txtptr++; stmt_deffn(); return; }
    if (peekc() == T_SEG) {
        txtptr++;
        if (accept('=')) def_seg = (unsigned)eval_long();
        else def_seg = hal_dataseg();
        return;
    }
    if (peekc() == T_USR) { txtptr = skip_stmt(txtptr, 1); return; }
    syntax();
}

static void stmt_deftype(int type)
{
    do {
        char a, b;
        a = (char)peekc(); if (!isalpha((u8)a)) syntax(); txtptr++;
        b = a;
        if (accept('-')) { b = (char)peekc(); if (!isalpha((u8)b)) syntax(); txtptr++; }
        set_deftype(a, b, type);
    } while (accept(','));
}

static void stmt_randomize(void)
{
    long n;
    if (at_stmt_end()) {
        char buf[64];
        for (;;) {
            con_puts("\x97\x90\x90\x94\x82\xCC\x8E\xED\x81i-32768\x81`32767\x81j\x3F ");  /* 乱数の種（-32768〜32767）? */
            if (con_lineinput(buf, sizeof(buf), 1) < 0) do_end(1);
            if (buf[0]) break;
        }
        n = atol(buf);
    } else n = (long)eval_dbl();
    rnd_randomize(n);
}

static void stmt_poke(void)
{
    long a; int v;
    a = eval_long(); expect(','); v = eval_int();
    hal_poke(def_seg, (unsigned)a, v & 0xFF);
}

static void stmt_out(void)
{
    long p; int v;
    p = eval_long(); expect(','); v = eval_int();
    hal_outp((unsigned)p, v & 0xFF);
}

/* XDD / XDE / XDF: stubs for PC-88 Z80-driver extension statements.
 * Arguments are evaluated (for validation) and discarded.  XDF takes a bare
 * command name (letters, stored as plain characters by the tokenizer) before
 * the list.  Set P8P_TRACE_EXT to log each call to stderr. */
/* Scratch area for synthesized PC-88 token streams (the Z80 extension
 * handlers parse their arguments from a token stream, so the relay
 * re-encodes the interpreter's evaluated arguments into one). */
#define EXT_STREAM 0x8F00

static unsigned ext_put(unsigned p, int byte)
{
    hal_poke(0, p, byte);
    return (p + 1) & 0xFFFF;
}

static unsigned ext_put_num(unsigned p, long v)
{
    unsigned u = (unsigned)(v & 0xFFFF);
    if (u <= 9) return ext_put(p, 0x11 + (int)u);   /* digit token */
    p = ext_put(p, 0x1C);                            /* int16 constant */
    p = ext_put(p, (int)(u & 0xFF));
    return ext_put(p, (int)(u >> 8));
}

/* Build a synthesized token stream for an extension statement:
 * [FF][sub][space] arg0 [, arg1] ... [00]. Numbers are re-encoded as
 * token literals; a string argument becomes a quoted literal. The
 * handlers parse their arguments out of the stream (via the RST 8
 * svc 0x2C trap), exactly as on hardware. Returns the address of the
 * first argument byte (what HL points at on handler entry). */
static unsigned ext_build_stream(int sub, const long *nums,
                                 const int *arg_is_str,
                                 const char (*arg_str)[128],
                                 const int *arg_slen, int narg,
                                 const int *arg_sep)
{
    unsigned p = EXT_STREAM;
    int i;
    p = ext_put(p, 0xFF);
    p = ext_put(p, sub);
    p = ext_put(p, 0x20);
    for (i = 0; i < narg; i++) {
        /* Preserve the source separator: NEOMSC's FFDD handler
         * dispatches on it -- ',' continues with a direct argument
         * (0x9F65), ';' introduces a data reference (song, type)
         * resolved through its song table. Flattening ';' to ','
         * sent data references down the direct path, where the song
         * index was misread as a string pointer and nothing plotted. */
        if (i) p = ext_put(p, arg_sep ? (unsigned)arg_sep[i] : 0x2C);
        if (arg_is_str[i]) {
            int j;
            p = ext_put(p, 0x22);
            for (j = 0; j < arg_slen[i]; j++)
                p = ext_put(p, (unsigned char)arg_str[i][j]);
            p = ext_put(p, 0x22);
        } else {
            p = ext_put_num(p, nums[i]);
        }
    }
    ext_put(p, 0x00);
    return EXT_STREAM + 3;
}

static void stmt_ext(int which)
{
    /* which: 0 = XDD, 1 = XDE, 2 = XDF, 3 = POLL. POLL is the
     * ALPHA-MINI spelling of the XDF channel (mcg hooks the same EEA7
     * vector and reads its command letters from (HL)), so 3 takes
     * the XDF path throughout and differs only in the name shown. */
    static const char *nm[4] = { "XDD", "XDE", "XDF", "POLL" };
    int letters = (which == 2 || which == 3);
    char cmd[16];
    long nums[16];
    int arg_sep[16];
    int pending_sep = 0x2C;
    int arg_is_str[16];
    static char arg_str[16][128];
    int arg_slen[16];
    int nnum = 0;
    int n = 0, args = 1;
    int tr = getenv("P8P_TRACE_EXT") != 0;
    cmd[0] = 0;
    if (letters) {
        while (*txtptr == ' ') txtptr++;
        while (isalpha(*txtptr)) {
            if (n < (int)sizeof(cmd) - 1) cmd[n++] = (char)toupper(*txtptr);
            txtptr++;
        }
        cmd[n] = 0;
        if (n) args = accept(',');
    }
    if (tr) {
        fprintf(stderr, "%s", nm[which]);
        if (letters) fprintf(stderr, " %s", cmd);
    }
    while (args) {
        VAL v;
        if (at_stmt_end()) break;
        eval(&v);
        if (nnum < 16) arg_sep[nnum] = pending_sep;
        if (nnum < 16) {
            if (v.type == T_STR) {
                int L = (int)v.u.s.len;
                if (L > 127) L = 127;
                arg_is_str[nnum] = 1;
                arg_slen[nnum] = L;
                if (L > 0 && v.u.s.p) memcpy(arg_str[nnum], v.u.s.p, (size_t)L);
                arg_str[nnum][L] = 0;
                nums[nnum] = 0;
            } else {
                arg_is_str[nnum] = 0;
                arg_slen[nnum] = 0;
                nums[nnum] = (long)val_to_dbl(&v);
            }
            nnum++;
        }
        if (tr) {
            if (v.type == T_STR)
                fprintf(stderr, " \"%.*s\"", (int)v.u.s.len, v.u.s.p ? v.u.s.p : "");
            else
                fprintf(stderr, " %g", val_to_dbl(&v));
        }
        if (accept(',')) { pending_sep = 0x2C; continue; }
        if (accept(';')) { pending_sep = 0x3B; continue; }
        break;
    }
    if (tr) fputc('\n', stderr);

    /* The relays jump to whatever handler the guest hooked into the
     * statement's EEA vector (vessel_relay_target reads the vector's
     * JP), as the genuine interpreter does. With no handler hooked
     * the statement does nothing for this call, and the vessel says
     * so on stderr once per vector. For NEOMSC the targets are
     * 0x9E5C / 0x994D / 0xA6DB (docs/NEOMSC-CONTRACT.md). */
    if (which == 0 && nnum >= 1) {
        /* XDD: relay to the FFDD handler (the EEA1 vector's target;
         * BC = the vector address, HL = the first argument token,
         * A = that token's byte — the convention the genuine
         * interpreter uses, verified against QUASI88). */
        unsigned target;
        if (vessel_relay_target(0xEEA1, nm[0], &target)) {
            unsigned hl = ext_build_stream(0xDD, nums, arg_is_str,
                                           arg_str, arg_slen, nnum, arg_sep);
            vessel_prime_arg(nums[0]);
            vessel_call(target, (unsigned)hal_peek(0, hl), hl, 0xEEA1, 0);
        }
    } else if (letters && cmd[0]) {
        /* XDF / POLL: relay to the FFDF handler (the EEA7 vector's target;
         * BC = the vector address, HL = the command letter, A = that
         * letter — docs/NEOMSC-CONTRACT.md. Stock NEOMSC reloads A
         * from (HL), but a handler dispatching on A needs it). POLL
         * builds the same stream, FF DF header included: mcg never
         * looks behind HL, so the guest protocol is identical. */
        unsigned target;
        if (vessel_relay_target(0xEEA7, nm[which], &target)) {
            unsigned p = EXT_STREAM, i;
            p = ext_put(p, 0xFF); p = ext_put(p, 0xDF); p = ext_put(p, 0x20);
            for (i = 0; cmd[i]; i++) p = ext_put(p, (unsigned char)cmd[i]);
            for (n = 0; n < nnum; n++) {
                p = ext_put(p, 0x2C);
                p = ext_put_num(p, nums[n]);
            }
            ext_put(p, 0x00);
            vessel_call(target, (unsigned char)cmd[0], EXT_STREAM + 3, 0xEEA7, 0);
        }
    } else if (which == 1 && nnum >= 1) {
        /* XDE services with statement-level equivalents in the
         * vessel: 0 = raw sector read into guest memory, 1 = the
         * ADPCM block bulk load into the SB2 sample RAM. The
         * second argument selects the bundled disk image (a .D88
         * file can hold a multi-disk set; MA2026's songs live on
         * image 1, everything else on image 0). Service 2
         * (DRAM -> guest copy) is unreachable in MA2026's shipped
         * configuration (MENU.ALP forces RD=1, so the per-song
         * loads always take the service 0 path), and service 3 is
         * the staff-roll text service; both stay trace-only. */
        if (nums[0] == 0 && nnum >= 6)
            vessel_disk_read((int)nums[1], (int)nums[2], (int)nums[3],
                             (int)nums[4], (unsigned)(nums[5] & 0xFFFF));
        else if (nums[0] == 1 && nnum >= 4)
            vessel_dram_load((int)nums[1], (int)nums[2], (int)nums[3]);
        else {
            /* Other XDE services (2 = DRAM copy, 3 = staff roll):
             * relay to the FFDE handler (the EEA4 vector's target;
             * BC = the vector address, A = the first argument's
             * token byte). HL deliberately differs from the genuine
             * entry value 0xE69C: the vessel has no ROM text pointer
             * of its own, so the RST 8 svc 0x2C / 0x11D3 traps parse
             * the following arguments from HL, which therefore has to
             * point at the first argument token as for XDD. */
            unsigned target;
            if (vessel_relay_target(0xEEA4, nm[1], &target)) {
                unsigned hl = ext_build_stream(0xDE, nums, arg_is_str,
                                               arg_str, arg_slen, nnum, arg_sep);
                vessel_prime_arg(nums[0]);
                vessel_call(target, (unsigned)hal_peek(0, hl), hl, 0xEEA4, 0);
            }
        }
    }
}

static void stmt_wait(void)
{
    long p; int a, x = 0;
    p = eval_long(); expect(','); a = eval_int();
    if (accept(',')) x = eval_int();
    while ((((hal_inp((unsigned)p) & 0xFF) ^ x) & a) == 0) {
        if (hal_breakflag()) do_end(1);
    }
}

static void stmt_call(void)
{
    long a;
    if (isalpha(peekc())) {
        int t; void *vp = var_ptr_for(0, &t, 1); VAL v;
        var_get(vp, t, &v);
        a = (long)val_to_dbl(&v);
        if (a < 0) a += 65536L;
    } else a = eval_long();
    if (accept('(')) { do { VAL dummy; eval(&dummy); } while (accept(',')); expect(')'); }
    hal_call(def_seg, (unsigned)a);
}

static void stmt_error(void)
{
    int n = eval_int();
    if (n < 1 || n > 255) berror(E_ILLEGAL_FN);
    berror(n);
}

static void stmt_beep(void)
{
    if (at_stmt_end()) {
        unsigned long t0 = hal_ticks();
        long guard = 0;
        hal_beep(1);
        while (hal_ticks() - t0 < 6 && ++guard < 2000000L) hal_idle();
        hal_beep(0);
        return;
    }
    hal_beep(eval_int() != 0);
}

static void stmt_sound(void)
{
    /* SOUND freq, duration : approximated with beeper on/off */
    int f, d;
    unsigned long t0;
    long guard = 0;
    f = eval_int(); expect(','); d = eval_int();
    if (f <= 0 || d <= 0) { hal_beep(0); return; }
    hal_beep(1);
    t0 = hal_ticks();
    while (hal_ticks() - t0 < (unsigned long)d * 60 / 18 && ++guard < 4000000L) hal_idle();
    hal_beep(0);
}

static void stmt_locate(void)
{
    int x = cur_x, y = cur_y, sw = -1;
    if (peekc() != ',') x = eval_int();
    if (accept(',')) { if (peekc() != ',' && !at_stmt_end()) y = eval_int(); if (accept(',')) sw = eval_int(); }
    if (x < 0 || x >= scr_w || y < 0 || y >= scr_h - (key_disp ? 1 : 0) + (key_disp ? 0 : 0)) {
        if (x < 0 || x >= scr_w || y < 0 || y >= scr_h) berror(E_ILLEGAL_FN);
    }
    con_locate(x, y);
    (void)sw;
}

static void stmt_cls(void)
{
    int n = 1;
    if (!at_stmt_end()) n = eval_int();
    if (n < 0 || n > 3) berror(E_ILLEGAL_FN);
    if (n == 1 || n == 3 || n == 0) scr_cls();
    if (n == 2 || n == 3) g_cls();
}

static void stmt_width(void)
{
    int w = scr_w, h = scr_h;
    if (peekc() == '"' || peekc() == '#') { txtptr = skip_stmt(txtptr, 1); return; }
    if (peekc() != ',') w = eval_int();
    if (accept(',')) h = eval_int();
    if ((w != 40 && w != 80) || (h != 20 && h != 25)) berror(E_ILLEGAL_FN);
    con_set_width(w, h);
}

static void stmt_console(void)
{
    int top = con_top, lines = con_bot - con_top + 1, ksw = key_disp;
    int csw = -1;
    if (peekc() != ',') top = eval_int();
    if (accept(',')) { if (peekc() != ',' && !at_stmt_end()) lines = eval_int();
        if (accept(',')) { if (peekc() != ',' && !at_stmt_end()) ksw = eval_int();
            if (accept(',')) csw = eval_int(); } }
    if (csw == 0) { con_mono = 1; text_attr = 0xE1; }      /* monochrome: COLOR = attribute */
    else if (csw > 0) { con_mono = 0; text_attr = 0xE1; }
    if (top < 0 || lines < 1 || top + lines > scr_h) berror(E_ILLEGAL_FN);
    con_console(top, lines, ksw);
}

static void stmt_time_date(int is_time)
{
    VAL v; int a, b, c;
    expect('=');
    eval_str(&v);
    if (v.u.s.len != 8) berror(E_ILLEGAL_FN);
    {
        char s[9]; memcpy(s, v.u.s.p, 8); s[8] = 0;
        if (sscanf(s, is_time ? "%d:%d:%d" : "%d/%d/%d", &a, &b, &c) != 3) berror(E_ILLEGAL_FN);
    }
    if (is_time) { if (a > 23 || b > 59 || c > 59) berror(E_ILLEGAL_FN); hal_settime(a, b, c); }
    else { if (b < 1 || b > 12 || c < 1 || c > 31) berror(E_ILLEGAL_FN); hal_setdate(a < 80 ? 2000 + a : 1900 + a, b, c); }
}

static void stmt_lset(int right)
{
    int t; void *vp; VAL cur, v;
    peekc(); vp = var_ptr_for(0, &t, 1);
    if (t != T_STR) berror(E_TYPE);
    expect('='); eval_str(&v);
    var_get(vp, t, &cur);
    if (cur.u.s.len) {
        char *p = str_temp(cur.u.s.len);
        unsigned n = v.u.s.len < cur.u.s.len ? v.u.s.len : cur.u.s.len;
        memset(p, ' ', cur.u.s.len);
        if (right) memcpy(p + cur.u.s.len - n, v.u.s.p, n); else memcpy(p, v.u.s.p, n);
        cur.u.s.p = p;
        var_set(vp, t, &cur);
    }
}

static void stmt_load(int merge)
{
    VAL f; char fn[80]; int run = 0;
    eval_str(&f);
    if (accept(',')) { if (toupper(peekc()) == 'R') { txtptr++; run = 1; } }
    file_fname(f.u.s.p, f.u.s.len, fn, ".BAS");
    if (!merge) { file_close_all(); }
    if (!prog_load(fn, merge)) berror(E_FILE_NF);
    if (!merge) var_clear_all();
    running = 0;
    if (run) { clear_runtime(); run_program(prog_mem); }
    longjmp(top_jmp, 3);
}

static void stmt_save(void)
{
    VAL f; char fn[80];
    eval_str(&f);
    if (accept(',')) { txtptr++; }   /* ,A / ,P accepted */
    file_fname(f.u.s.p, f.u.s.len, fn, ".BAS");
    if (!prog_save(fn)) berror(E_IO);
}

static void stmt_chain(void)
{
    VAL f; char fn[80]; u16 start = 0; int has_start = 0;
    if (accept(T_MERGE)) {}
    eval_str(&f);
    if (accept(',')) { if (has_linenum()) { start = get_linenum(); has_start = 1; } else if (!at_stmt_end() && peekc() != ',') { start = (u16)eval_long(); has_start = 1; } }
    while (!at_stmt_end()) txtptr = skip_token(txtptr);
    file_fname(f.u.s.p, f.u.s.len, fn, ".BAS");
    if (!prog_load(fn, 0)) berror(E_FILE_NF);
    sp = 0; data_lp = 0; data_p = 0; fn_clear();
    {
        u8 *l = has_start ? find_line(start, 1) : prog_mem;
        if (!l) berror(E_UNDEF_LINE);
        run_program(l);
    }
    longjmp(top_jmp, 3);
}

static void stmt_edit(void)
{
    u16 n;
    u8 *l;
    char buf[512];
    int k;
    if (accept('.')) n = err_line; else n = get_linenum();
    l = find_line(n, 1);
    if (!l) berror(E_UNDEF_LINE);
    k = sprintf(buf, "%u ", n);
    detokenize_line(l + 4, buf + k, sizeof(buf) - k);
    con_puts(buf);
    {
        /* move cursor to the beginning of that logical line */
        int len = (int)strlen(buf);
        int rows = (len + scr_w - 1) / scr_w; if (rows < 1) rows = 1;
        int y = cur_y - (cur_x == 0 ? rows : rows - 1);
        if (y < con_top) y = con_top;
        con_locate(0, y);
    }
    ok_needed = 0;
    longjmp(top_jmp, 4);
}

void stmt_key(void)
{
    if (accept(T_ON)) { con_keydisp(1); return; }
    if (accept(T_OFF)) { con_keydisp(0); return; }
    if (accept(T_LIST)) {
        int i;
        for (i = 0; i < 10; i++) {
            char b[40]; int j, o = 0;
            o = sprintf(b, "f%d ", i + 1);
            for (j = 0; fkey_str[i][j]; j++) {
                u8 c = (u8)fkey_str[i][j];
                b[o++] = (char)(c < 0x20 ? ' ' : c);
            }
            b[o] = 0;
            con_puts(b); con_newline();
        }
        return;
    }
    if (accept('(')) {
        int n = eval_int();
        expect(')');
        if (accept(T_ON)) key_stmt_trap(n, 1);
        else if (accept(T_OFF)) key_stmt_trap(n, 0);
        else if (accept(T_STOP)) key_stmt_trap(n, 2);
        else syntax();
        return;
    }
    {
        int n = eval_int(); VAL s;
        expect(',');
        eval_str(&s);
        if (n < 1 || n > 10) berror(E_ILLEGAL_FN);
        {
            unsigned len = s.u.s.len > 15 ? 15 : s.u.s.len;
            memcpy(fkey_str[n - 1], s.u.s.p, len); fkey_str[n - 1][len] = 0;
        }
        con_key_redraw();
    }
}

static void stmt_auto(void)
{
    auto_line = 10; auto_step = 10;
    if (has_linenum()) auto_line = get_linenum();
    else if (accept('.')) auto_line = (err_line == 0xFFFF ? 10 : err_line);
    if (accept(',')) { if (has_linenum()) auto_step = get_linenum(); else auto_step = (u16)eval_int(); }
    if (auto_step == 0) berror(E_ILLEGAL_FN);
    auto_mode = 1;
    longjmp(top_jmp, 2);
}

static void stmt_option(void)
{
    int b;
    expect(T_BASE);
    b = eval_int();
    if (b != 0 && b != 1) syntax();
    option_base_set(b);
}

/* ---------------- statement dispatch ---------------- */
static void exec_stmt(void)
{
    u8 c = peekc();
    if (c == '*') {                 /* label definition: nothing to execute */
        txtptr++;
        while (label_char(*txtptr)) txtptr++;
        return;
    }
    if (c < 0x80 || c == TK_FUNC) {
        if (isalpha(c)) { stmt_let(); end_of_stmt(); return; }
        if (c == TK_FUNC) syntax();
        syntax();
    }
    txtptr++;
    switch (c) {
    case T_END: if (!running && curline == 0xFFFF) { file_close_all(); return; } do_end(0); return;
    case T_STOP: do_end(1); return;
    case T_FOR: stmt_for(); return;
    case T_NEXT: stmt_next(); return;
    case T_DATA: case T_REM: txtptr = skip_stmt(txtptr - 1, 0); if (c == T_REM) while (*txtptr) txtptr++; return;
    case T_INPUT: stmt_input(0); break;
    case T_DIM: stmt_dim(); break;
    case T_READ: stmt_read(); break;
    case T_LET: stmt_let(); break;
    case T_GOTO: goto_line(get_linenum()); return;
    case T_RUN: stmt_run(); return;
    case T_IF: stmt_if(); return;
    case T_RESTORE: stmt_restore(); break;
    case T_GOSUB: {
        u16 n = get_linenum();
        u8 *save;
        if (!find_line(n, 1)) berror(E_UNDEF_LINE);
        save = txtptr;
        push_gosub(0);
        (void)save;
        goto_line(n);
        return; }
    case T_RETURN: stmt_return(); return;
    case T_PRINT: stmt_print(0); break;
    case T_LPRINT: { FILE *save = out_fp; stmt_print((FILE *)-1); out_fp = save; break; }
    case T_CLEAR: stmt_clear(); break;
    case T_LIST: stmt_list(); break;
    case T_LLIST: { u16 a, b; parse_range(&a, &b); break; }
    case T_NEW: prog_new(); running = 0; longjmp(top_jmp, 2);
    case T_ON: stmt_on(); return;
    case T_WAIT: stmt_wait(); break;
    case T_DEF: stmt_def(); return;
    case T_POKE: stmt_poke(); break;
    case T_CONT: stmt_cont(); return;
    case T_OUT: stmt_out(); break;
    case T_XDD: stmt_ext(0); break;
    case T_XDE: stmt_ext(1); break;
    case T_XDF: stmt_ext(2); break;
    case T_POLL: stmt_ext(3); break;
    case T_CLS: stmt_cls(); break;
    case T_WIDTH: stmt_width(); break;
    case T_ELSE: while (*txtptr) txtptr = skip_token(txtptr); return;
    case T_TRON: trace_on = 1; break;
    case T_TROFF: trace_on = 0; break;
    case T_SWAP: stmt_swap(); break;
    case T_ERASE: stmt_erase(); break;
    case T_ERROR: stmt_error(); return;
    case T_RESUME: stmt_resume(); return;
    case T_DELETE: stmt_delete(); break;
    case T_AUTO: stmt_auto(); return;
    case T_RENUM: stmt_renum(); break;
    case T_DEFSTR: stmt_deftype(T_STR); break;
    case T_DEFINT: stmt_deftype(T_INT); break;
    case T_DEFSNG: stmt_deftype(T_SNG); break;
    case T_DEFDBL: stmt_deftype(T_DBL); break;
    case T_LINE:
        if (accept(T_INPUT)) { stmt_input(1); break; }
        stmt_line(); break;
    case T_WHILE: stmt_while(); return;
    case T_WEND: stmt_wend(); return;
    case T_CALL: stmt_call(); break;
    case T_WRITE: {
        FILE *fp = 0; int first = 1;
        if (accept('#')) { int n = eval_int(); fp = file_get(n, 'O'); if (!accept(',')) { fputs("\r\n", fp); break; } }
        out_fp = fp;
        while (!at_stmt_end()) {
            VAL v; char b[40];
            if (!first) out_char(',');
            first = 0;
            eval(&v);
            if (v.type == T_STR) { out_char('"'); out_str(v.u.s.p, v.u.s.len); out_char('"'); }
            else { char *p; num_to_str(&v, b); p = b; if (*p == ' ') p++; out_str(p, strlen(p)); }
            if (!accept(',')) break;
        }
        if (fp) fputs("\r\n", fp); else con_newline();
        out_fp = 0;
        break; }
    case T_COMMON: txtptr = skip_stmt(txtptr, 1); break;
    case T_CHAIN: stmt_chain(); return;
    case T_OPTION: stmt_option(); break;
    case T_RANDOMIZE: stmt_randomize(); break;
    case T_SYSTEM: file_close_all(); hal_term(); exit(0);
    case T_OPEN: stmt_open(); break;
    case T_FIELD: berror(E_ILLEGAL_FN); break;
    case T_GET: if (accept('@')) { stmt_getput_gfx(0); break; } berror(E_ILLEGAL_FN); break;
    case T_PUT: if (accept('@')) { stmt_getput_gfx(1); break; } berror(E_ILLEGAL_FN); break;
    case T_CLOSE: stmt_close(); break;
    case T_LOAD: stmt_load(0); return;
    case T_MERGE: stmt_load(1); return;
    case T_FILES: stmt_files(); break;
    case T_NAME: stmt_name(); break;
    case T_KILL: stmt_kill(); break;
    case T_LSET: stmt_lset(0); break;
    case T_RSET: stmt_lset(1); break;
    case T_SAVE: stmt_save(); break;
    case T_LOCATE: stmt_locate(); break;
    case T_SCREEN: stmt_screen(); break;
    case T_COLOR: stmt_color(); break;
    case T_PSET: stmt_pset(0); break;
    case T_PRESET: stmt_pset(1); break;
    case T_CIRCLE: stmt_circle(); break;
    case T_PAINT: stmt_paint(); break;
    case T_VIEW: stmt_view(); break;
    case T_WINDOW: stmt_window(); break;
    case T_CONSOLE: stmt_console(); break;
    case T_KEY: stmt_key(); break;
    case T_BEEP: stmt_beep(); break;
    case T_SOUND: stmt_sound(); break;
    case T_BLOAD: stmt_bload(); break;
    case T_BSAVE: stmt_bsave(); break;
    case T_EDIT: stmt_edit(); return;
    case T_MID: stmt_mid_assign(); break;
    case T_TIME: stmt_time_date(1); break;
    case T_DATE: stmt_time_date(0); break;
    case T_PLAY: txtptr = skip_stmt(txtptr, 1); break;
    default: syntax();
    }
    end_of_stmt();
}

/* ---------------- direct mode ---------------- */
void exec_direct(const char *line)
{
    int n = tokenize(line, direct_buf, sizeof(direct_buf));
    (void)n;
    txtptr = direct_buf;
    curline = 0xFFFF;
    running = 0;
    exec_loop();
}

/* handle a line typed in direct mode */
static void process_line(const char *s)
{
    while (*s == ' ') s++;
    if (isdigit((u8)*s)) {
        long n = 0;
        u8 tok[300];
        int len;
        while (isdigit((u8)*s)) { n = n * 10 + (*s - '0'); s++; if (n > 65529L) berror(E_SYNTAX); }
        if (*s == ' ') s++;
        len = tokenize(s, tok, sizeof(tok));
        if (len <= 1) {
            if (auto_mode) return;
            if (!find_line((u16)n, 1)) berror(E_UNDEF_LINE);
            prog_delete_range((u16)n, (u16)n);
        } else prog_insert((u16)n, tok, len);
        if (auto_mode) auto_line = (u16)(n + auto_step);
        ok_needed = 0;
        return;
    }
    if (*s == 0) { ok_needed = 0; return; }
    ok_needed = 1;
    exec_direct(s);
}

static const char *pending_cmd = 0;
void exec_direct_pending_set(const char *s) { pending_cmd = s; }

void top_level(void)
{
    char buf[300];
    int r;
    r = setjmp(top_jmp);
    if (r) {
        running = 0;
        run_jmp_valid = 0;
        curline = 0xFFFF;
        str_temp_reset();
        out_fp = 0;
        if (r == 1) { ok_needed = 1; }
        if (r == 4) ok_needed = 0;
        else if (r != 2 || !auto_mode) ok_needed = 1;
        if (exit_after_run && r == 3) { hal_term(); exit(0); }
        exit_after_run = 0;
    }
    for (;;) {
        int res;
        running = 0;
        curline = 0xFFFF;
        str_temp_reset();
        if (ok_needed && !auto_mode) {
            if (cur_x) con_newline();
            con_puts("Ok"); con_newline();
        }
        ok_needed = 0;
        if (auto_mode) {
            char nb[12];
            if (cur_x) con_newline();
            sprintf(nb, "%u%c", auto_line, find_line(auto_line, 1) ? '*' : ' ');
            con_puts(nb);
        }
        if (pending_cmd) {
            const char *pc = pending_cmd;
            pending_cmd = 0;
            process_line(pc);
            continue;
        }
        /* Direct mode: about to wait for a command line. The ROM is
         * modelled as coming back here with a fresh display state,
         * so a program that left the overlay register (port 0x53)
         * hiding TEXT - then ended, stopped or errored out - does
         * not leave the Ok prompt and the line editor's echo
         * invisible. This is the only direct-mode read: a running
         * program never comes through here (its INPUT / LINE INPUT
         * call con_lineinput themselves, from stmt_input), and the
         * queued boot "RUN" above is dispatched without reaching it. */
        hal_pile_reset();
        res = con_lineinput(buf, sizeof(buf), 0);
        if (res < 0) {   /* STOP / CTRL+C */
            if (auto_mode) { auto_mode = 0; ok_needed = 1; }
            continue;
        }
        if (auto_mode) {
            /* strip leading '*' after number */
            char *p = buf; while (*p == ' ') p++;
            while (isdigit((u8)*p)) p++;
            if (*p == '*') *p = ' ';
        }
        process_line(buf);
        if (!ok_needed) continue;
    }
}

/* ---------------- LOAD / SAVE ---------------- */
int prog_load(const char *fname, int merge)
{
    FILE *fp = fopen(fname, "rb");
    char line[400];
    int n = 0, c;
    if (!fp) return 0;
    c = fgetc(fp);
    if (c == 0xFF) { fclose(fp); berror(E_BAD_MODE); }
    if (c != EOF) ungetc(c, fp);
    if (!merge) { prog_mem[0] = prog_mem[1] = 0; invalidate(); }
    for (;;) {
        c = fgetc(fp);
        if (c == EOF || c == 0x1A || c == '\n' || c == '\r') {
            line[n] = 0;
            if (n > 0) {
                const char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                if (*s) {
                    if (!isdigit((u8)*s)) { fclose(fp); berror(E_DIRECT_IN_FILE); }
                    {
                        long ln = 0; u8 tok[300]; int len;
                        while (isdigit((u8)*s)) { ln = ln * 10 + (*s - '0'); s++; }
                        if (*s == ' ') s++;
                        if (ln > 65529L) { fclose(fp); berror(E_SYNTAX); }
                        len = tokenize(s, tok, sizeof(tok));
                        if (len > 1) prog_insert((u16)ln, tok, len);
                    }
                }
            }
            n = 0;
            if (c == EOF || c == 0x1A) break;
            continue;
        }
        if (c == '\t') c = ' ';
        if (n < (int)sizeof(line) - 1) line[n++] = (char)c;
    }
    fclose(fp);
    return 1;
}

/* The boot program's load, issued before top_level runs. berror
 * longjmps to top_jmp, which only top_level arms, so a load made
 * ahead of it needs its own landing pad - and even a well-sniffed
 * file can still raise from tokenize. Returns prog_load's result
 * (0: the file did not open), or -1 when a berror landed here; the
 * error is already on the screen by then. The direct-mode state is
 * set first, as top_level does: an earlier session of this process
 * that bailed mid-RUN must not send berror to its dead run_jmp. */
int prog_load_boot(const char *fname)
{
    running = 0;
    run_jmp_valid = 0;
    curline = 0xFFFF;
    if (setjmp(top_jmp)) return -1;
    return prog_load(fname, 0);
}

int prog_save(const char *fname)
{
    FILE *fp = fopen(fname, "wb");
    if (!fp) return 0;
    list_program(0, 65529u, fp);
    fputc(0x1A, fp);
    fclose(fp);
    return 1;
}

/* Runs once per session (never per RUN). A process can host several
 * sessions in turn (engine.c), so state that a RUN does not clear
 * is put back to its initial value here. */
void interp_init(void)
{
    free(prog_mem);             /* the previous session's program area (NULL on first init) */
    prog_mem = (u8 *)malloc(PROG_MAX + 4);
    if (!prog_mem) { puts("\x83\x81\x83\x82\x83\x8A\x82\xAA\x91\xAB\x82\xE8\x82\xDC\x82\xB9\x82\xF1");  /* メモリが足りません */ exit(1); }
    prog_mem[0] = prog_mem[1] = 0;
    p8_mem_top = 0;             /* CLEAR ,m not given yet */
    rnd_reset();
    eval_reset_fn_depth();      /* a bail inside a DEF FN body leaves it raised */
    def_seg = hal_dataseg();
}
