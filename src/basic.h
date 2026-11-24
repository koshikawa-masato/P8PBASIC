/* basic.h - P9PBASIC common definitions */
#ifndef BASIC_H
#define BASIC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "hal.h"

#define VERSION_STR "P9PBASIC ver 261120"

typedef short i16;
typedef unsigned short u16;
typedef long i32;
typedef unsigned char u8;

/* value types (MS BASIC VALTYP convention) */
#define T_INT 2
#define T_STR 3
#define T_SNG 4
#define T_DBL 8

typedef struct {
    u8 type;
    union { i16 i; float f; double d; struct { u16 len; char *p; } s; } u;
} VAL;

/* ---------- tokens ---------- */
#define TK_LINENUM 0x01   /* + 2 bytes */
#define TK_OCT     0x06   /* + 2 bytes */
#define TK_HEX     0x05   /* + 2 bytes */
#define TK_INT     0x02   /* + 2 bytes */
#define TK_SNG     0x03   /* + 4 bytes */
#define TK_DBL     0x04   /* + 8 bytes */
#define TK_FUNC    0xFE   /* + 1 byte function code */

enum {
    T_ALL = 0x81, T_AND, T_APOS, T_APPEND, T_AS, T_AUTO, T_BASE, T_BEEP, T_BLOAD, T_BSAVE, T_CALL, T_CHAIN, T_CIRCLE, T_CLEAR, T_CLOSE, T_CLS, T_COLOR, T_COMMON, T_CONSOLE, T_CONT, T_DATA, T_DATE, T_DEF, T_DEFDBL, T_DEFINT, T_DEFSNG, T_DEFSTR, T_DELETE, T_DIM, T_EDIT, T_ELSE, T_END, T_EQV, T_ERASE, T_ERL, T_ERR, T_ERROR, T_FIELD, T_FILES, T_FN, T_FOR, T_GET, T_GOSUB, T_GOTO, T_IF, T_IMP, T_INPUT, T_KEY, T_KILL, T_LET, T_LINE, T_LIST, T_LLIST, T_LOAD, T_LOCATE, T_LPRINT, T_LSET, T_MERGE, T_MID, T_MOD, T_NAME, T_NEW, T_NEXT, T_NOT, T_OFF, T_ON, T_OPEN, T_OPTION, T_OR, T_OUT, T_OUTPUT, T_PAINT, T_PLAY, T_POKE, T_PRESET, T_PRINT, T_PSET, T_PUT, T_RANDOMIZE, T_READ, T_REM, T_RENUM, T_RESTORE, T_RESUME, T_RETURN, T_RSET, T_RUN, T_SAVE, T_SCREEN, T_SEG, T_SOUND, T_SPC, T_STEP, T_STOP, T_SWAP, T_SYSTEM, T_TAB, T_THEN, T_TIME, T_TO, T_TROFF, T_TRON, T_USING, T_USR, T_VIEW, T_WAIT, T_WEND, T_WHILE, T_WIDTH, T_WINDOW, T_WRITE, T_XOR,
    T_XDD, T_XDE, T_XDF,
    T_POLL,
    T_LAST_STMT
};

enum {
    F_ABS = 0x81, F_ASC, F_ATN, F_ATTR, F_CDBL, F_CHR, F_CINT, F_COS, F_CSNG, F_CSRLIN, F_CVD, F_CVI, F_CVS, F_DSKF, F_EOF, F_EXP, F_FIX, F_FRE, F_HEX, F_INKEY, F_INP, F_INPUTS, F_INSTR, F_INT, F_JIS, F_KINSTR, F_KLEN, F_KMID, F_KNJ, F_KTYPE, F_LEFT, F_LEN, F_LOC, F_LOF, F_LOG, F_LPOS, F_MKD, F_MKI, F_MKS, F_OCT, F_PEEK, F_POINT, F_POS, F_RIGHT, F_RND, F_SGN, F_SIN, F_SPACE, F_SQR, F_STICK, F_STR, F_STRIG, F_STRING, F_TAN, F_VAL, F_VARPTR,
    F_LAST
};

/* ---------- error codes ---------- */
#define E_NEXT_WO_FOR   1
#define E_SYNTAX        2
#define E_RET_WO_GOSUB  3
#define E_OUT_OF_DATA   4
#define E_ILLEGAL_FN    5
#define E_OVERFLOW      6
#define E_OUT_OF_MEM    7
#define E_UNDEF_LINE    8
#define E_SUBSCRIPT     9
#define E_DUP_DEF      10
#define E_DIV0         11
#define E_ILLEGAL_DIRECT 12
#define E_TYPE         13
#define E_OUT_STR      14
#define E_STR_LONG     15
#define E_STR_COMPLEX  16
#define E_CANT_CONT    17
#define E_UNDEF_FN     18
#define E_NO_RESUME    19
#define E_RESUME_WO_ERR 20
#define E_MISSING_OP   22
#define E_LINE_OVF     23
#define E_FOR_WO_NEXT  26
#define E_WHILE_WO_WEND 29
#define E_WEND_WO_WHILE 30
#define E_FIELD_OVF    50
#define E_INTERNAL     51
#define E_BAD_FILENO   52
#define E_FILE_NF      53
#define E_BAD_MODE     54
#define E_FILE_OPEN    55
#define E_IO           57
#define E_FILE_EXISTS  58
#define E_DISK_FULL    61
#define E_INPUT_PAST   62
#define E_BAD_RECNO    63
#define E_BAD_FNAME    64
#define E_DIRECT_IN_FILE 66
#define E_TOO_MANY_FILES 67
#define E_RENAME_ACROSS 74

/* ---------- interpreter state ---------- */
extern u8 *txtptr;          /* execution pointer */
extern u16 curline;         /* 0xFFFF = direct */
extern jmp_buf err_jmp;
extern int running;

void berror(int code);
void syntax(void);

/* token helpers */
u8   peekc(void);           /* skip spaces, return current byte */
#define PEEKC() (*txtptr == ' ' ? peekc() : *txtptr)
u8   getc_tok(void);        /* skip spaces, return and advance */
void expect(u8 c);
int  accept(u8 c);
int  accept_func(u8 f);
int  at_stmt_end(void);

/* tokenizer */
int  tokenize(const char *src, u8 *dst, int dstmax);
void detokenize_line(const u8 *tok, char *out, int outmax);
const char *keyword_name(int tok, int func);

/* expression */
void eval(VAL *v);
void eval_num(VAL *v);
int  eval_int(void);          /* -32768..32767 */
long eval_long(void);         /* for addresses 0..65535 etc */
double eval_dbl(void);
void eval_str(VAL *v);
int  val_to_int(VAL *v);
double val_to_dbl(VAL *v);
void val_conv(VAL *v, int type);
double round_away(double x);

/* string temp pool */
char *str_temp(unsigned len);
void  str_temp_reset(void);
void  str_temp_keep(char *p);
char *str_dup_heap(const char *p, unsigned len);

/* variables */
typedef struct VAR VAR;
VAR *var_find(const char *name, int type, int create);
void *var_ptr_for(u8 **pp, int *ptype, int create_array_auto); /* parse name[(subs)] */
void var_get(void *p, int type, VAL *v);
void var_set(void *p, int type, VAL *v);
int  parse_varname(char *name, int *type);
void var_clear_all(void);
void var_erase(const char *name, int type);
void dim_array(const char *name, int type, int ndim, int *dims);
int  default_type(char c);
void set_deftype(char from, char to, int type);
extern int option_base;
void option_base_set(int b);
void *array_base(const char *name, int type, long *bytes);

/* numbers */
void num_to_str(VAL *v, char *buf);   /* with leading sign space */
int  str_to_num(const char *s, VAL *v, int *used);

/* program */
u8  *find_line(u16 num, int exact);
u8  *prog_first(void);
u8  *line_next(u8 *l);
u16  line_num(u8 *l);
void prog_insert(u16 num, u8 *tok, int len);
void prog_delete_range(u16 a, u16 b);
void prog_new(void);
int  prog_load(const char *fname, int merge);
int  prog_load_boot(const char *fname);   /* -1: berror during the load */
int  prog_save(const char *fname);
void exec_direct(const char *line);
void run_program(u8 *start);
void top_level(void);
unsigned prog_free(void);
extern u8 *prog_mem;
extern unsigned prog_size;
extern int auto_mode;
extern u16 auto_line, auto_step;
extern u16 err_line;
extern int err_code;
extern int exit_after_run;

/* screen / console */
extern int scr_w, scr_h, cur_x, cur_y, con_top, con_bot, key_disp;
extern unsigned char text_attr;
extern int con_mono;
void scr_init(void);
void scr_cls(void);
void con_putc(int c);
void con_puts(const char *s);
void con_newline(void);
void con_locate(int x, int y);
void con_scroll_up(void);
void con_set_width(int w, int h);
void con_console(int top, int lines, int keysw);
void con_keydisp(int on);
void con_key_redraw(void);
int  con_getchar(int wait);
int  con_inkey(void);
int  con_lineinput(char *buf, int max, int is_input);
unsigned short scr_code(int x, int y);
extern char fkey_str[10][16];
void con_flush_keys(void);

/* graphics */
void g_init(void);
void stmt_screen(void);
void stmt_pset(int preset);
void stmt_line(void);
void stmt_circle(void);
void stmt_paint(void);
void stmt_color(void);
void stmt_view(void);
void stmt_window(void);
void stmt_getput_gfx(int is_put);
int  fn_point(void);
void g_cls(void);
extern int g_fg, g_bg;

/* file I/O */
void stmt_open(void);
void stmt_close(void);
void file_close_all(void);
void stmt_files(void);
void stmt_kill(void);
void stmt_name(void);
void stmt_bload(void);
void stmt_bsave(void);
FILE *file_get(int n, int mode);  /* mode: 'I','O',0 any */
int  parse_filenum(void);
int  fn_eof(int n);
long fn_lof(int n);
long fn_loc(int n);
void file_fname(const char *in, unsigned len, char *out, const char *defext);
void print_to(FILE *fp);
extern FILE *out_fp;  /* NULL = console */

/* print */
void stmt_print(FILE *fp);
void out_char(int c);
void out_str(const char *s, unsigned len);
extern int out_col;

/* misc */
void list_program(u16 a, u16 b, FILE *fp);
extern unsigned def_seg;
extern unsigned p8_mem_top;   /* CLEAR ,m (0 = not set) */
extern int trace_on;
void stmt_key(void);
void key_traps_check(void);
extern int ok_needed;

#endif
