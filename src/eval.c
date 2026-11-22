/* eval.c - expression evaluation and built-in functions */
#include "basic.h"
#include "vessel.h"
#include <ctype.h>
#include <math.h>

void *array_base(const char *name, int type, long *bytes);

/* ---------------- string temp pool ---------------- */
#define POOLMAX 256
static char *pool[POOLMAX];
static int npool;

char *str_temp(unsigned len)
{
    char *p;
    if (len > 255) berror(E_STR_LONG);
    if (npool >= POOLMAX) berror(E_STR_COMPLEX);
    p = (char *)malloc(len ? len : 1);
    if (!p) berror(E_OUT_STR);
    pool[npool++] = p;
    return p;
}

void str_temp_reset(void)
{
    while (npool > 0) free(pool[--npool]);
}

/* ---------------- helpers ---------------- */
double round_away(double x)
{
    return x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5);
}

void val_conv(VAL *v, int type)
{
    if (v->type == type) return;
    if (v->type == T_STR || type == T_STR) berror(E_TYPE);
    switch (type) {
    case T_INT: {
        double d = (v->type == T_SNG) ? v->u.f : v->u.d;
        d = round_away(d);
        if (d < -32768.0 || d > 32767.0) berror(E_OVERFLOW);
        v->u.i = (i16)d;
        break; }
    case T_SNG: {
        double d = (v->type == T_INT) ? v->u.i : v->u.d;
        if (d > 1.701411e38 || d < -1.701411e38) berror(E_OVERFLOW);
        v->u.f = (float)d;
        break; }
    case T_DBL:
        v->u.d = (v->type == T_INT) ? (double)v->u.i : (double)v->u.f;
        break;
    }
    v->type = (u8)type;
}

double val_to_dbl(VAL *v)
{
    switch (v->type) {
    case T_INT: return v->u.i;
    case T_SNG: return v->u.f;
    case T_DBL: return v->u.d;
    }
    berror(E_TYPE);
    return 0;
}

int val_to_int(VAL *v)
{
    VAL t = *v;
    val_conv(&t, T_INT);
    return t.u.i;
}

/* for logical operators: accept -32768..65535 */
static int to_logic(VAL *v)
{
    double d;
    if (v->type == T_INT) return v->u.i;
    if (v->type == T_STR) berror(E_TYPE);
    d = round_away(val_to_dbl(v));
    if (d < -32768.0 || d > 65535.0) berror(E_OVERFLOW);
    return (i16)(u16)(long)d;
}

static void set_num(VAL *v, int type, double d)
{
    v->type = (u8)type;
    if (type == T_DBL) v->u.d = d;
    else if (type == T_SNG) {
        if (d > 1.701411e38 || d < -1.701411e38) berror(E_OVERFLOW);
        v->u.f = (float)d;
    } else {
        if (d < -32768.0 || d > 32767.0) berror(E_OVERFLOW);
        v->u.i = (i16)d;
    }
}

static void need_num(VAL *v) { if (v->type == T_STR) berror(E_TYPE); }
static void need_str(VAL *v) { if (v->type != T_STR) berror(E_TYPE); }

static void make_str(VAL *v, const char *p, unsigned len)
{
    char *q = str_temp(len);
    memcpy(q, p, len);
    v->type = T_STR; v->u.s.len = (u16)len; v->u.s.p = q;
}

/* ---------------- RND ----------------
 * Own generator: 32-bit xorshift (shifts 13/17/5), upper 24 bits -> [0,1).
 */
#define RND_SEED0 0x2545F491UL
static unsigned long rnd_state = RND_SEED0;
static float rnd_last = 0;

/* back to the power-on sequence (interp_init: once per session) */
void rnd_reset(void)
{
    rnd_state = RND_SEED0;
    rnd_last = 0;
}

static float rnd_next(void)
{
    unsigned long x = rnd_state;
    x ^= (x << 13) & 0xFFFFFFFFUL;
    x ^= x >> 17;
    x ^= (x << 5) & 0xFFFFFFFFUL;
    rnd_state = x & 0xFFFFFFFFUL;
    rnd_last = (float)((double)(rnd_state >> 8) / 16777216.0);
    return rnd_last;
}

void rnd_randomize(long n)
{
    unsigned long s = ((unsigned long)n & 0xFFFFUL) * 2654435761UL + 0x9E3779B9UL;
    s &= 0xFFFFFFFFUL;
    if (s == 0) s = 1;
    rnd_state = s;
}

/* ---------------- MBF conversion ---------------- */
static void ieee_to_mbf4(float f, u8 *o)
{
    u8 b[4]; int exp, sign;
    memcpy(b, &f, 4);
    sign = b[3] & 0x80;
    exp = ((b[3] & 0x7F) << 1) | (b[2] >> 7);
    if (exp == 0) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    exp = exp - 127 + 129;
    o[0] = b[0]; o[1] = b[1];
    o[2] = (u8)((b[2] & 0x7F) | sign);
    o[3] = (u8)exp;
}

static float mbf4_to_ieee(const u8 *m)
{
    u8 b[4]; int exp; float f;
    if (m[3] == 0) return 0.0f;
    exp = m[3] - 129 + 127;
    if (exp <= 0) return 0.0f;
    b[0] = m[0]; b[1] = m[1];
    b[2] = (u8)((m[2] & 0x7F) | ((exp & 1) << 7));
    b[3] = (u8)((m[2] & 0x80) | (exp >> 1));
    memcpy(&f, b, 4);
    return f;
}

static void ieee_to_mbf8(double d, u8 *o)
{
    /* MBF double: 7 bytes mantissa (56 bits incl. hidden), exponent byte */
    int e, i;
    double m;
    int sign = d < 0;
    if (d == 0) { memset(o, 0, 8); return; }
    if (sign) d = -d;
    m = frexp(d, &e);          /* d = m * 2^e, 0.5<=m<1 */
    o[7] = (u8)(e + 128);
    m = m * 2.0 - 1.0;         /* remove hidden bit (m in [0,1)) */
    /* produce 55 bits of fraction */
    {
        u8 bytes[7];
        double f = m;
        for (i = 6; i >= 0; i--) {
            int bits = (i == 6) ? 7 : 8;
            f *= (double)(1 << bits);
            bytes[i] = (u8)(int)f;
            f -= (int)f;
        }
        for (i = 0; i < 7; i++) o[i] = bytes[i];
        o[6] = (u8)((o[6] & 0x7F) | (sign ? 0x80 : 0));
    }
}

static double mbf8_to_ieee(const u8 *m)
{
    double f = 0;
    int i, e;
    if (m[7] == 0) return 0;
    e = m[7] - 128;
    f = (m[6] & 0x7F);
    for (i = 5; i >= 0; i--) f = f * 256.0 + m[i];
    f = f / 72057594037927936.0 * 2.0; /* /2^56 *2 -> fraction / 2^55 */
    f = (1.0 + f) * 0.5;
    f = ldexp(f, e);
    return (m[6] & 0x80) ? -f : f;
}

/* ---------------- user functions (DEF FN) ---------------- */
typedef struct FNDEF {
    struct FNDEF *next;
    char name[41]; int type;
    int nparam; char pname[8][41]; int ptype[8];
    u8 *body;
} FNDEF;
static FNDEF *fnlist;

void fn_clear(void)
{
    while (fnlist) { FNDEF *n = fnlist->next; free(fnlist); fnlist = n; }
}

void stmt_deffn(void)
{
    FNDEF *f, *g;
    char name[41]; int type;
    if (curline == 0xFFFF) berror(E_ILLEGAL_DIRECT);
    parse_varname(name, &type);
    f = (FNDEF *)calloc(1, sizeof(FNDEF));
    if (!f) berror(E_OUT_OF_MEM);
    strcpy(f->name, name); f->type = type;
    if (accept('(')) {
        do {
            if (f->nparam >= 8) { free(f); syntax(); }
            PEEKC();
            parse_varname(f->pname[f->nparam], &f->ptype[f->nparam]);
            f->nparam++;
        } while (accept(','));
        expect(')');
    }
    expect('=');
    f->body = txtptr;
    /* skip to statement end */
    {
        int q = 0;
        while (*txtptr && (q || *txtptr != ':')) {
            u8 c = *txtptr;
            if (c == '"') q = !q;
            if (!q) {
                if (c == TK_FUNC) txtptr++;
                else if (c == TK_INT || c == TK_LINENUM || c == TK_HEX || c == TK_OCT) txtptr += 2;
                else if (c == TK_SNG) txtptr += 4;
                else if (c == TK_DBL) txtptr += 8;
                else if (c == T_ELSE) break;
            }
            txtptr++;
        }
    }
    /* replace existing */
    for (g = fnlist; g; g = g->next)
        if (g->type == type && strcmp(g->name, name) == 0) {
            g->nparam = f->nparam; memcpy(g->pname, f->pname, sizeof(f->pname));
            memcpy(g->ptype, f->ptype, sizeof(f->ptype)); g->body = f->body;
            free(f); return;
        }
    f->next = fnlist; fnlist = f;
}

static void call_fn(VAL *res)
{
    char name[41]; int type, i;
    FNDEF *f;
    VAL args[8], saved[8];
    void *vp[8];
    u8 *save_ptr;
    static int depth = 0;
    PEEKC();
    parse_varname(name, &type);
    for (f = fnlist; f; f = f->next)
        if (f->type == type && strcmp(f->name, name) == 0) break;
    if (!f) berror(E_UNDEF_FN);
    if (f->nparam) {
        expect('(');
        for (i = 0; i < f->nparam; i++) {
            eval(&args[i]);
            if ((args[i].type == T_STR) != (f->ptype[i] == T_STR)) berror(E_TYPE);
            if (i < f->nparam - 1) expect(',');
        }
        expect(')');
    }
    if (++depth > 20) { depth = 0; berror(E_OUT_OF_MEM); }
    for (i = 0; i < f->nparam; i++) {
        u8 tmp[48]; u8 *sp = txtptr;
        int t;
        void *p;
        strcpy((char *)tmp, f->pname[i]);
        strcat((char *)tmp, f->ptype[i] == T_INT ? "%" : f->ptype[i] == T_SNG ? "!" :
               f->ptype[i] == T_DBL ? "#" : "$");
        txtptr = tmp;
        p = var_ptr_for(0, &t, 0);
        txtptr = sp;
        vp[i] = p;
        var_get(p, t, &saved[i]);
        if (t == T_STR) { /* keep a temp copy of old string */
            VAL c; make_str(&c, saved[i].u.s.p, saved[i].u.s.len); saved[i] = c;
        }
        var_set(p, t, &args[i]);
    }
    save_ptr = txtptr;
    txtptr = f->body;
    eval(res);
    txtptr = save_ptr;
    for (i = 0; i < f->nparam; i++) var_set(vp[i], f->ptype[i], &saved[i]);
    depth--;
    if (f->type == T_STR) need_str(res);
    else { need_num(res); val_conv(res, f->type); }
}

/* ---------------- primary ---------------- */
static void e_imp(VAL *v);

static void paren_expr(VAL *v) { expect('('); e_imp(v); expect(')'); }
static int paren_int(void) { int n; expect('('); n = eval_int(); expect(')'); return n; }

static void sjis_next(const u8 *p, unsigned len, unsigned *i)
{
    u8 c = p[*i];
    if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && *i + 1 < len) *i += 2;
    else *i += 1;
}

extern int fn_stick(int n);
extern int fn_strig(int n);
extern long dos_diskfree(int drive);

static void func(u8 f, VAL *v)
{
    VAL a, b;
    double d;
    int n, m;
    char buf[64];
    switch (f) {
    case F_ABS: paren_expr(v); need_num(v);
        if (v->type == T_INT) { if (v->u.i < 0) { if (v->u.i == -32768) set_num(v, T_SNG, 32768.0); else v->u.i = -v->u.i; } }
        else if (v->type == T_SNG) v->u.f = (float)fabs(v->u.f); else v->u.d = fabs(v->u.d);
        return;
    case F_SGN: paren_expr(v); need_num(v); d = val_to_dbl(v);
        v->type = T_INT; v->u.i = d > 0 ? 1 : d < 0 ? -1 : 0; return;
    case F_INT: paren_expr(v); need_num(v);
        if (v->type == T_SNG) v->u.f = (float)floor(v->u.f); else if (v->type == T_DBL) v->u.d = floor(v->u.d);
        return;
    case F_FIX: paren_expr(v); need_num(v);
        if (v->type == T_SNG) { d = v->u.f; v->u.f = (float)(d < 0 ? ceil(d) : floor(d)); }
        else if (v->type == T_DBL) { d = v->u.d; v->u.d = d < 0 ? ceil(d) : floor(d); }
        return;
    case F_CINT: paren_expr(v); need_num(v); val_conv(v, T_INT); return;
    case F_CSNG: paren_expr(v); need_num(v); val_conv(v, T_SNG); return;
    case F_CDBL: paren_expr(v); need_num(v); val_conv(v, T_DBL); return;
    case F_SIN: case F_COS: case F_TAN: case F_ATN: case F_EXP: case F_LOG: case F_SQR: {
        int t;
        paren_expr(v); need_num(v);
        t = v->type == T_DBL ? T_DBL : T_SNG;
        d = val_to_dbl(v);
        switch (f) {
        case F_SIN: d = sin(d); break;
        case F_COS: d = cos(d); break;
        case F_TAN: d = tan(d); break;
        case F_ATN: d = atan(d); break;
        case F_EXP: if (d > 88.02969) berror(E_OVERFLOW); d = exp(d); break;
        case F_LOG: if (d <= 0) berror(E_ILLEGAL_FN); d = log(d); break;
        case F_SQR: if (d < 0) berror(E_ILLEGAL_FN); d = sqrt(d); break;
        }
        set_num(v, t, d);
        return; }
    case F_RND: {
        float r;
        if (PEEKC() == '(') {
            paren_expr(&a); need_num(&a); d = val_to_dbl(&a);
            if (d < 0) { rnd_randomize((long)(d * 1000.0)); r = rnd_next(); }
            else if (d == 0) r = rnd_last;
            else r = rnd_next();
        } else r = rnd_next();
        v->type = T_SNG; v->u.f = r;
        return; }
    case F_LEN: paren_expr(v); need_str(v); n = v->u.s.len; v->type = T_INT; v->u.i = (i16)n; return;
    case F_ASC: paren_expr(v); need_str(v); if (!v->u.s.len) berror(E_ILLEGAL_FN);
        n = (u8)v->u.s.p[0]; v->type = T_INT; v->u.i = (i16)n; return;
    case F_CHR: n = paren_int(); if (n < 0 || n > 255) berror(E_ILLEGAL_FN);
        buf[0] = (char)n; make_str(v, buf, 1); return;
    case F_STR: paren_expr(&a); need_num(&a); num_to_str(&a, buf); make_str(v, buf, strlen(buf)); return;
    case F_VAL: {
        char *s; int used = 0, neg = 0; unsigned i = 0;
        paren_expr(&a); need_str(&a);
        s = (char *)malloc(a.u.s.len + 1);
        if (!s) berror(E_OUT_OF_MEM);
        memcpy(s, a.u.s.p, a.u.s.len); s[a.u.s.len] = 0;
        while (s[i] == ' ' || s[i] == '\t') i++;
        if (s[i] == '-') { neg = 1; i++; } else if (s[i] == '+') i++;
        if (s[i] == '&') {
            long lv = 0; int hex = (s[i+1] == 'H' || s[i+1] == 'h');
            i += (s[i+1] == 'H' || s[i+1] == 'h' || s[i+1] == 'O' || s[i+1] == 'o') ? 2 : 1;
            while (s[i]) {
                int c = toupper((u8)s[i]);
                if (hex && isxdigit(c)) lv = lv * 16 + (isdigit(c) ? c - '0' : c - 'A' + 10);
                else if (!hex && c >= '0' && c <= '7') lv = lv * 8 + (c - '0');
                else break;
                i++;
            }
            v->type = T_INT; v->u.i = (i16)(u16)lv;
            if (neg) v->u.i = -v->u.i;
        } else if (isdigit((u8)s[i]) || s[i] == '.') {
            /* VAL never raises overflow-int: use double then pick type */
            int tt;
            str_to_num(s + i, v, &used);
            tt = v->type;
            if (neg) {
                if (tt == T_INT) v->u.i = -v->u.i;
                else if (tt == T_SNG) v->u.f = -v->u.f;
                else v->u.d = -v->u.d;
            }
        } else { v->type = T_INT; v->u.i = 0; }
        free(s);
        return; }
    case F_HEX: case F_OCT: {
        u16 u;
        paren_expr(&a); need_num(&a); u = (u16)to_logic(&a);
        sprintf(buf, f == F_HEX ? "%X" : "%o", u);
        make_str(v, buf, strlen(buf)); return; }
    case F_SPACE: n = paren_int(); if (n < 0 || n > 255) berror(E_ILLEGAL_FN);
        { char *p = str_temp(n); memset(p, ' ', n); v->type = T_STR; v->u.s.len = (u16)n; v->u.s.p = p; }
        return;
    case F_STRING: {
        char *p; int c;
        expect('('); n = eval_int(); expect(',');
        eval(&a);
        if (a.type == T_STR) { if (!a.u.s.len) berror(E_ILLEGAL_FN); c = (u8)a.u.s.p[0]; }
        else c = val_to_int(&a);
        expect(')');
        if (n < 0 || n > 255 || c < 0 || c > 255) berror(E_ILLEGAL_FN);
        p = str_temp(n); memset(p, c, n);
        v->type = T_STR; v->u.s.len = (u16)n; v->u.s.p = p;
        return; }
    case F_LEFT: case F_RIGHT:
        expect('('); eval(&a); need_str(&a); expect(','); n = eval_int(); expect(')');
        if (n < 0 || n > 255) berror(E_ILLEGAL_FN);
        if (n > a.u.s.len) n = a.u.s.len;
        *v = a; v->u.s.len = (u16)n;
        if (f == F_RIGHT) v->u.s.p = a.u.s.p + (a.u.s.len - n);
        return;
    case F_INSTR: case F_KINSTR: {
        int start = 1; unsigned i;
        expect('('); eval(&a);
        if (a.type != T_STR) { start = val_to_int(&a); if (start < 1 || start > 255) berror(E_ILLEGAL_FN); expect(','); eval(&a); }
        need_str(&a); expect(','); eval(&b); need_str(&b); expect(')');
        v->type = T_INT; v->u.i = 0;
        if (f == F_KINSTR) {
            /* character-based (SJIS aware) */
            unsigned pos = 0; int cno = 1;
            while (pos < a.u.s.len) {
                if (cno >= start && pos + b.u.s.len <= a.u.s.len && memcmp(a.u.s.p + pos, b.u.s.p, b.u.s.len) == 0) { v->u.i = (i16)cno; return; }
                sjis_next((u8 *)a.u.s.p, a.u.s.len, &pos); cno++;
            }
            return;
        }
        if (start > a.u.s.len) return;
        if (b.u.s.len == 0) { v->u.i = (i16)start; return; }
        for (i = start - 1; i + b.u.s.len <= a.u.s.len; i++)
            if (memcmp(a.u.s.p + i, b.u.s.p, b.u.s.len) == 0) { v->u.i = (i16)(i + 1); return; }
        return; }
    case F_KLEN: {
        unsigned pos = 0; int cnt = 0;
        paren_expr(&a); need_str(&a);
        while (pos < a.u.s.len) { sjis_next((u8 *)a.u.s.p, a.u.s.len, &pos); cnt++; }
        v->type = T_INT; v->u.i = (i16)cnt; return; }
    case F_KMID: {
        unsigned pos = 0, st; int cno = 1, cnt = 255;
        expect('('); eval(&a); need_str(&a); expect(','); n = eval_int();
        if (accept(',')) cnt = eval_int();
        expect(')');
        if (n < 1 || cnt < 0) berror(E_ILLEGAL_FN);
        while (pos < a.u.s.len && cno < n) { sjis_next((u8 *)a.u.s.p, a.u.s.len, &pos); cno++; }
        st = pos;
        while (pos < a.u.s.len && cnt > 0) { sjis_next((u8 *)a.u.s.p, a.u.s.len, &pos); cnt--; }
        v->type = T_STR; v->u.s.p = a.u.s.p + st; v->u.s.len = (u16)(pos - st);
        return; }
    case F_KNJ: {    /* KNJ$("JIS hex") -> SJIS char */
        unsigned jis, c1, c2;
        paren_expr(&a); need_str(&a);
        memcpy(buf, a.u.s.p, a.u.s.len > 8 ? 8 : a.u.s.len); buf[a.u.s.len > 8 ? 8 : a.u.s.len] = 0;
        jis = (unsigned)strtol(buf, 0, 16);
        c1 = (jis >> 8) & 0x7F; c2 = jis & 0x7F;
        if (c1 < 0x21 || c2 < 0x21) berror(E_ILLEGAL_FN);
        buf[0] = (char)(((c1 + 1) >> 1) + (c1 <= 0x5E ? 0x70 : 0xB0));
        buf[1] = (char)(c2 + ((c1 & 1) ? (c2 >= 0x60 ? 0x20 : 0x1F) : 0x7E));
        make_str(v, buf, 2); return; }
    case F_JIS: {
        unsigned c1, c2;
        paren_expr(&a); need_str(&a);
        if (a.u.s.len < 2) berror(E_ILLEGAL_FN);
        c1 = (u8)a.u.s.p[0]; c2 = (u8)a.u.s.p[1];
        c1 -= (c1 <= 0x9F) ? 0x71 : 0xB1; c1 = c1 * 2 + 1;
        if (c2 > 0x7F) c2--;
        if (c2 >= 0x9E) { c2 -= 0x7D; c1++; } else c2 -= 0x1F;
        sprintf(buf, "%04X", (c1 << 8) | c2);
        make_str(v, buf, 4); return; }
    case F_KTYPE: { /* KTYPE(s$,n): 0 = ANK, 1 = kanji */
        unsigned pos = 0; int cno = 1;
        expect('('); eval(&a); need_str(&a); expect(','); n = eval_int(); expect(')');
        while (pos < a.u.s.len && cno < n) { sjis_next((u8 *)a.u.s.p, a.u.s.len, &pos); cno++; }
        v->type = T_INT;
        if (pos >= a.u.s.len) berror(E_ILLEGAL_FN);
        { u8 c = (u8)a.u.s.p[pos]; v->u.i = ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) ? 1 : 0; }
        return; }
    case F_PEEK: { long ad; expect('('); ad = eval_long(); expect(')');
        v->type = T_INT; v->u.i = (i16)hal_peek(def_seg, (unsigned)ad); return; }
    /* INP_CHARGE_CYCLES: the ROM's work per INP() evaluation (function
     * dispatch, argument and result conversion) beyond the flat
     * statement cost. Calibrated against the genuine machine's key
     * polling rate (docs/APPROXIMATIONS.md item 35). */
#define INP_CHARGE_CYCLES 12000L
    case F_INP: { long p; expect('('); p = eval_long(); expect(')');
        vessel_charge(INP_CHARGE_CYCLES);
        v->type = T_INT; v->u.i = (i16)(hal_inp((unsigned)p) & 0xFF); return; }
    case F_FRE: paren_expr(&a);
        v->type = T_SNG; v->u.f = (float)prog_free(); return;
    case F_POS: paren_expr(&a); v->type = T_INT; v->u.i = (i16)cur_x; return;
    case F_LPOS: paren_expr(&a); v->type = T_INT; v->u.i = 0; return;
    case F_CSRLIN: v->type = T_INT; v->u.i = (i16)cur_y; return;
    case F_INKEY: {
        int c = con_inkey();
        if (c < 0) { v->type = T_STR; v->u.s.len = 0; v->u.s.p = str_temp(0); }
        else { buf[0] = (char)c; make_str(v, buf, 1); }
        return; }
    case F_INPUTS: {
        int fno = 0, i; char *p;
        expect('('); n = eval_int();
        if (accept(',')) { accept('#'); fno = eval_int(); }
        expect(')');
        if (n < 0 || n > 255) berror(E_ILLEGAL_FN);
        p = str_temp(n);
        if (fno) {
            FILE *fp = file_get(fno, 'I');
            for (i = 0; i < n; i++) { int c = fgetc(fp); if (c == EOF) berror(E_INPUT_PAST); p[i] = (char)c; }
        } else {
            for (i = 0; i < n; i++) { int c = con_getchar(1); p[i] = (char)c; }
        }
        v->type = T_STR; v->u.s.len = (u16)n; v->u.s.p = p;
        return; }
    case F_MKI: paren_expr(&a); need_num(&a); n = val_to_int(&a);
        buf[0] = (char)(n & 0xFF); buf[1] = (char)((n >> 8) & 0xFF); make_str(v, buf, 2); return;
    case F_MKS: paren_expr(&a); need_num(&a); val_conv(&a, T_SNG);
        ieee_to_mbf4(a.u.f, (u8 *)buf); make_str(v, buf, 4); return;
    case F_MKD: paren_expr(&a); need_num(&a); val_conv(&a, T_DBL);
        ieee_to_mbf8(a.u.d, (u8 *)buf); make_str(v, buf, 8); return;
    case F_CVI: paren_expr(&a); need_str(&a); if (a.u.s.len < 2) berror(E_ILLEGAL_FN);
        v->type = T_INT; v->u.i = (i16)((u8)a.u.s.p[0] | ((u8)a.u.s.p[1] << 8)); return;
    case F_CVS: paren_expr(&a); need_str(&a); if (a.u.s.len < 4) berror(E_ILLEGAL_FN);
        v->type = T_SNG; v->u.f = mbf4_to_ieee((u8 *)a.u.s.p); return;
    case F_CVD: paren_expr(&a); need_str(&a); if (a.u.s.len < 8) berror(E_ILLEGAL_FN);
        v->type = T_DBL; v->u.d = mbf8_to_ieee((u8 *)a.u.s.p); return;
    case F_EOF: n = paren_int(); v->type = T_INT; v->u.i = (i16)(fn_eof(n) ? -1 : 0); return;
    case F_LOF: n = paren_int(); set_num(v, T_SNG, (double)fn_lof(n)); return;
    case F_LOC: n = paren_int(); set_num(v, T_SNG, (double)fn_loc(n)); return;
    case F_POINT: v->type = T_INT; v->u.i = (i16)fn_point(); return;
    case F_VARPTR: {
        int t; void *p; unsigned long sp;
        expect('(');
        if (accept('#')) { n = eval_int(); expect(')'); file_get(n, 0); v->type = T_INT; v->u.i = 0; return; }
        PEEKC();
        p = var_ptr_for(0, &t, 0);
        n = 0;
        if (accept(',')) n = eval_int();
        expect(')');
        sp = hal_varptr(p);
        v->type = T_INT;
        v->u.i = (i16)(u16)(n == 1 ? (sp >> 16) : (sp & 0xFFFF));
        return; }
    case F_STICK: n = paren_int(); v->type = T_INT; v->u.i = (i16)fn_stick(n); return;
    case F_STRIG: n = paren_int(); v->type = T_INT; v->u.i = (i16)fn_strig(n); return;
    case F_DSKF: n = paren_int(); set_num(v, T_SNG, (double)dos_diskfree(n)); return;
    case F_ATTR: {   /* ATTR$(n): file attribute string - minimal */
        paren_expr(&a); make_str(v, "", 0); return; }
    default: break;
    }
    (void)m; (void)b;
    syntax();
}

static void e_atom(VAL *v)
{
    u8 c = PEEKC();
    switch (c) {
    case TK_INT: case TK_HEX: case TK_OCT:
        txtptr++;
        v->type = T_INT; v->u.i = (i16)(txtptr[0] | (txtptr[1] << 8));
        txtptr += 2;
        return;
    case TK_LINENUM:   /* treat as number */
        txtptr++;
        set_num(v, T_SNG, (double)(u16)(txtptr[0] | (txtptr[1] << 8)));
        txtptr += 2;
        return;
    case TK_SNG:
        txtptr++; v->type = T_SNG; memcpy(&v->u.f, txtptr, 4); txtptr += 4; return;
    case TK_DBL:
        txtptr++; v->type = T_DBL; memcpy(&v->u.d, txtptr, 8); txtptr += 8; return;
    case '"': {
        u8 *s = ++txtptr;
        while (*txtptr && *txtptr != '"') txtptr++;
        v->type = T_STR; v->u.s.p = (char *)s; v->u.s.len = (u16)(txtptr - s);
        if (*txtptr == '"') txtptr++;
        return; }
    case '(':
        txtptr++; e_imp(v); expect(')'); return;
    case TK_FUNC:
        txtptr++; func(*txtptr++, v); return;
    case T_MID: {
        VAL a; int n, m = 255;
        txtptr++;
        expect('('); eval(&a); need_str(&a); expect(','); n = eval_int();
        if (accept(',')) m = eval_int();
        expect(')');
        if (n < 1 || n > 255 || m < 0 || m > 255) berror(E_ILLEGAL_FN);
        *v = a;
        if (n > a.u.s.len) { v->u.s.len = 0; return; }
        v->u.s.p = a.u.s.p + n - 1;
        v->u.s.len = (u16)((a.u.s.len - (n - 1)) < (unsigned)m ? (a.u.s.len - (n - 1)) : (unsigned)m);
        return; }
    case T_TIME: {
        int h, m, s; char b[12];
        txtptr++; hal_gettime(&h, &m, &s);
        sprintf(b, "%02d:%02d:%02d", h, m, s); make_str(v, b, 8); return; }
    case T_DATE: {
        int y, mo, d; char b[12];
        txtptr++; hal_getdate(&y, &mo, &d);
        sprintf(b, "%02d/%02d/%02d", y % 100, mo, d); make_str(v, b, 8); return; }
    case T_ERR: txtptr++; v->type = T_INT; v->u.i = (i16)err_code; return;
    case T_ERL: txtptr++; set_num(v, err_line > 32767 ? T_SNG : T_INT, (double)(err_line == 0xFFFF ? 65535u : err_line)); return;
    case T_FN: txtptr++; call_fn(v); return;
    case T_USR: berror(E_ILLEGAL_FN); return;
    default:
        if (isalpha(c)) {
            int t; void *p = var_ptr_for(0, &t, 1);
            var_get(p, t, v);
            return;
        }
        if (c == 0 || c == ':' || c == T_ELSE) berror(E_MISSING_OP);
        syntax();
    }
}

static void negate(VAL *v)
{
    need_num(v);
    if (v->type == T_INT) { if (v->u.i == -32768) set_num(v, T_SNG, 32768.0); else v->u.i = -v->u.i; }
    else if (v->type == T_SNG) v->u.f = -v->u.f;
    else v->u.d = -v->u.d;
}

static int maxtype(VAL *a, VAL *b) { return a->type > b->type ? a->type : b->type; }

/* operator ids */
enum { OP_NONE, OP_IMP, OP_EQV, OP_XOR, OP_OR, OP_AND, OP_REL, OP_ADD, OP_SUB,
       OP_MOD, OP_IDIV, OP_MUL, OP_DIV, OP_POW };
static const u8 op_prec[] = { 0, 1, 2, 3, 4, 5, 7, 8, 8, 9, 10, 11, 11, 13 };

static void e_bin(VAL *v, int minprec);

/* peek operator at txtptr; returns OP id and length via *len, relmask via *rel */
static int peek_op(int *len, int *rel)
{
    u8 c = PEEKC();
    *len = 1;
    switch (c) {
    case '+': return OP_ADD;
    case '-': return OP_SUB;
    case '*': return OP_MUL;
    case '/': return OP_DIV;
    case '\\': return OP_IDIV;
    case '^': return OP_POW;
    case T_MOD: return OP_MOD;
    case T_AND: return OP_AND;
    case T_OR: return OP_OR;
    case T_XOR: return OP_XOR;
    case T_EQV: return OP_EQV;
    case T_IMP: return OP_IMP;
    case '<': case '=': case '>': {
        int op = 0, n = 0;
        u8 *p = txtptr;
        for (;;) {
            u8 ch = *p;
            int bit;
            if (ch == ' ') { p++; n++; continue; }
            bit = ch == '<' ? 1 : ch == '=' ? 2 : ch == '>' ? 4 : 0;
            if (!bit || (op & bit)) break;
            op |= bit; p++; n++;
        }
        *len = n; *rel = op;
        return OP_REL;
    }
    }
    return OP_NONE;
}

static void e_unary(VAL *v)
{
    u8 c = PEEKC();
    if (c == '-') { txtptr++; e_bin(v, 12); negate(v); return; }
    if (c == '+') { txtptr++; e_bin(v, 12); need_num(v); return; }
    if (c == T_NOT) {
        int x;
        txtptr++;
        e_bin(v, 7);
        x = to_logic(v); v->type = T_INT; v->u.i = (i16)~x;
        return;
    }
    e_atom(v);
}

static void apply(VAL *v, int op, int rel, VAL *b)
{
    int t;
    switch (op) {
    case OP_POW: {
        double x, y, r;
        need_num(v); need_num(b);
        t = (v->type == T_DBL || b->type == T_DBL) ? T_DBL : T_SNG;
        x = val_to_dbl(v); y = val_to_dbl(b);
        if (x == 0 && y < 0) berror(E_DIV0);
        if (x < 0 && y != floor(y)) berror(E_ILLEGAL_FN);
        if (y == floor(y) && fabs(y) < 64) {
            long n = (long)fabs(y); r = 1;
            while (n--) r *= x;
            if (y < 0) r = 1 / r;
        } else r = pow(x, y);
        set_num(v, t, r);
        return; }
    case OP_MUL: case OP_DIV:
        need_num(v); need_num(b);
        t = maxtype(v, b);
        if (t == T_SNG) {
            float x = v->type == T_SNG ? v->u.f : (float)v->u.i;
            float y = b->type == T_SNG ? b->u.f : (float)b->u.i;
            float r;
            if (op == OP_MUL) r = x * y;
            else { if (y == 0) berror(E_DIV0); r = x / y; }
            if ((((u16 *)&r)[1] & 0x7F80) == 0x7F80) berror(E_OVERFLOW);
            v->type = T_SNG; v->u.f = r;
            return;
        }
        if (op == OP_MUL) {
            if (t == T_INT) {
                long r = (long)v->u.i * b->u.i;
                if (r >= -32768L && r <= 32767L) { v->u.i = (i16)r; return; }
                set_num(v, T_SNG, (double)r); return;
            }
            set_num(v, t, val_to_dbl(v) * val_to_dbl(b));
        } else {
            double y = val_to_dbl(b);
            if (y == 0) berror(E_DIV0);
            if (t == T_INT) {
                float r = (float)v->u.i / (float)b->u.i;
                v->type = T_SNG; v->u.f = r; return;
            }
            set_num(v, t, val_to_dbl(v) / y);
        }
        return;
    case OP_IDIV: case OP_MOD: {
        int x, y;
        need_num(v); need_num(b);
        x = val_to_int(v); y = val_to_int(b);
        if (y == 0) berror(E_DIV0);
        if (op == OP_IDIV) {
            if (x == -32768 && y == -1) berror(E_OVERFLOW);
            v->type = T_INT; v->u.i = (i16)(x / y);
        } else { v->type = T_INT; v->u.i = (i16)(x % y); }
        return; }
    case OP_ADD: case OP_SUB:
        if (v->type == T_STR || b->type == T_STR) {
            char *p;
            if (op != OP_ADD || v->type != T_STR || b->type != T_STR) berror(E_TYPE);
            if ((unsigned)v->u.s.len + b->u.s.len > 255) berror(E_STR_LONG);
            p = str_temp(v->u.s.len + b->u.s.len);
            memcpy(p, v->u.s.p, v->u.s.len);
            memcpy(p + v->u.s.len, b->u.s.p, b->u.s.len);
            v->u.s.p = p; v->u.s.len = (u16)(v->u.s.len + b->u.s.len);
            return;
        }
        t = maxtype(v, b);
        if (t == T_INT) {
            long r = (op == OP_ADD) ? (long)v->u.i + b->u.i : (long)v->u.i - b->u.i;
            if (r >= -32768L && r <= 32767L) { v->u.i = (i16)r; return; }
            set_num(v, T_SNG, (double)r); return;
        }
        if (t == T_SNG) {
            float x = v->type == T_SNG ? v->u.f : (float)v->u.i;
            float y = b->type == T_SNG ? b->u.f : (float)b->u.i;
            float r = (op == OP_ADD) ? x + y : x - y;
            if ((((u16 *)&r)[1] & 0x7F80) == 0x7F80) berror(E_OVERFLOW);
            v->type = T_SNG; v->u.f = r;
            return;
        }
        set_num(v, t, op == OP_ADD ? val_to_dbl(v) + val_to_dbl(b) : val_to_dbl(v) - val_to_dbl(b));
        return;
    case OP_REL: {
        int r;
        if (v->type == T_STR || b->type == T_STR) {
            unsigned n;
            int cmp;
            if (v->type != T_STR || b->type != T_STR) berror(E_TYPE);
            n = v->u.s.len < b->u.s.len ? v->u.s.len : b->u.s.len;
            cmp = memcmp(v->u.s.p, b->u.s.p, n);
            if (cmp == 0) cmp = (v->u.s.len > b->u.s.len) - (v->u.s.len < b->u.s.len);
            r = cmp < 0 ? 1 : cmp == 0 ? 2 : 4;
        } else {
            t = maxtype(v, b);
            if (t == T_INT) { i16 x = v->u.i, y = b->u.i; r = x < y ? 1 : x == y ? 2 : 4; }
            else if (t == T_SNG) {
                float x = v->type == T_SNG ? v->u.f : (float)v->u.i;
                float y = b->type == T_SNG ? b->u.f : (float)b->u.i;
                r = x < y ? 1 : x == y ? 2 : 4;
            } else {
                double x = val_to_dbl(v), y = val_to_dbl(b);
                r = x < y ? 1 : x == y ? 2 : 4;
            }
        }
        v->type = T_INT; v->u.i = (rel & r) ? -1 : 0;
        return; }
    default: {
        int x = to_logic(v), y = to_logic(b), r = 0;
        switch (op) {
        case OP_AND: r = x & y; break;
        case OP_OR:  r = x | y; break;
        case OP_XOR: r = x ^ y; break;
        case OP_EQV: r = ~(x ^ y); break;
        case OP_IMP: r = (~x) | y; break;
        }
        v->type = T_INT; v->u.i = (i16)r;
        return; }
    }
}

static void e_bin(VAL *v, int minprec)
{
    e_unary(v);
    for (;;) {
        int len, rel = 0, op, p;
        VAL b;
        op = peek_op(&len, &rel);
        if (op == OP_NONE) return;
        p = op_prec[op];
        if (p < minprec) return;
        txtptr += len;
        if (op == OP_POW) e_bin(&b, 14);       /* right operand: atom or unary */
        else e_bin(&b, p + 1);
        apply(v, op, rel, &b);
    }
}

static void e_imp(VAL *v) { e_bin(v, 1); }

void eval(VAL *v) { e_imp(v); }
void eval_num(VAL *v) { e_imp(v); need_num(v); }
void eval_str(VAL *v) { e_imp(v); need_str(v); }

int eval_int(void)
{
    VAL v; e_imp(&v); need_num(&v); return val_to_int(&v);
}

long eval_long(void)
{
    VAL v; double d;
    e_imp(&v); need_num(&v);
    d = round_away(val_to_dbl(&v));
    if (d < -32768.0 || d > 65535.0) berror(E_OVERFLOW);
    if (d < 0) d += 65536.0;
    return (long)d;
}

double eval_dbl(void)
{
    VAL v; e_imp(&v); need_num(&v); return val_to_dbl(&v);
}
