/* print.c - PRINT / PRINT USING */
#include "basic.h"
#include "vessel.h"
#include <ctype.h>
#include <math.h>

FILE *out_fp = 0;
int out_col = 0;

#define LPT ((FILE *)-1)

/* The ROM's text output work per character put on the screen, beyond
 * the flat statement cost; stmt_print charges it once per statement.
 * Calibrated together with INP and LINE (docs/APPROXIMATIONS.md
 * item 35). */
#define PRINT_CHAR_CYCLES 1500L
static long con_chars = 0;      /* screen characters emitted (newlines included) */

void out_char(int c)
{
    if (out_fp == LPT) return;
    if (out_fp) {
        if (c == '\n') { fputc('\r', out_fp); fputc('\n', out_fp); out_col = 0; return; }
        fputc(c, out_fp);
        out_col++;
        return;
    }
    con_chars++;
    con_putc(c & 0xFF);
}

void out_str(const char *s, unsigned len)
{
    unsigned i;
    for (i = 0; i < len; i++) out_char((u8)s[i]);
}

static int col(void) { return out_fp ? out_col : cur_x; }
static int width(void) { return out_fp ? 255 : scr_w; }

static void newline(void)
{
    if (out_fp == LPT) return;
    if (out_fp) { fputs("\r\n", out_fp); out_col = 0; }
    else { con_chars++; con_newline(); }
}

/* ---------------- PRINT USING ---------------- */
static int is_numfmt_start(const char *f, int i, int n)
{
    char c = f[i];
    if (c == '#') return 1;
    if (c == '.' && i + 1 < n && f[i + 1] == '#') return 1;
    if (c == '+' && i + 1 < n && (f[i + 1] == '#' || f[i + 1] == '.' || f[i + 1] == '*' || f[i + 1] == '$')) return 1;
    if (c == '*' && i + 1 < n && f[i + 1] == '*') return 1;
    if (c == '$' && i + 1 < n && f[i + 1] == '$') return 1;
    return 0;
}

static void fmt_number(const char *f, int *pi, int n, double x)
{
    int i = *pi;
    int lead_plus = 0, stars = 0, dollar = 0, comma = 0;
    int before = 0, after = 0, hasdot = 0, expo = 0, trail = 0;
    char out[80], digits[64];
    int o = 0, k, neg = x < 0;
    if (f[i] == '+') { lead_plus = 1; i++; }
    if (i + 1 < n && f[i] == '*' && f[i + 1] == '*') { stars = 1; before += 2; i += 2; if (i < n && f[i] == '$') { dollar = 1; i++; before++; } }
    else if (i + 1 < n && f[i] == '$' && f[i + 1] == '$') { dollar = 1; before += 1; i += 2; }
    while (i < n && (f[i] == '#' || f[i] == ',')) { if (f[i] == ',') comma = 1; before++; i++; }
    if (i < n && f[i] == '.') { hasdot = 1; i++; while (i < n && f[i] == '#') { after++; i++; } }
    if (i + 3 < n && f[i] == '^' && f[i+1] == '^' && f[i+2] == '^' && f[i+3] == '^') { expo = 1; i += 4; }
    if (i < n && (f[i] == '+' || f[i] == '-') && !lead_plus) { trail = f[i]; i++; }
    *pi = i;
    if (neg) x = -x;
    if (expo) {
        int e = 0, nd = before - (lead_plus || trail ? 0 : 1);
        if (nd < 1) nd = 1;
        if (x != 0) {
            e = (int)floor(log10(x)) - nd + 1;
            x = x / pow(10.0, e);
            if (floor(x * pow(10.0, after) + 0.5) >= pow(10.0, nd + after)) { x /= 10; e++; }
        }
        sprintf(digits, "%.*f", after, x);
        if (!hasdot) { char *d = strchr(digits, '.'); if (d) *d = 0; }
        o = 0;
        if (lead_plus) out[o++] = neg ? '-' : '+';
        else if (!trail && neg) out[o++] = '-';
        else if (!trail) out[o++] = ' ';
        for (k = (int)strlen(digits); k < nd + (hasdot ? after + 1 : 0); k++) out[o++] = ' ';
        strcpy(out + o, digits); o += (int)strlen(digits);
        o += sprintf(out + o, "E%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
        if (trail == '+') out[o++] = neg ? '-' : '+';
        else if (trail == '-') out[o++] = neg ? '-' : ' ';
        out[o] = 0;
        out_str(out, o);
        return;
    }
    {
        double sc = pow(10.0, after);
        x = floor(x * sc + 0.5) / sc;
    }
    sprintf(digits, "%.*f", after, x);
    /* insert commas */
    {
        char ip[64], fp[32], *d = strchr(digits, '.');
        int il, j, w = 0;
        if (d) { *d = 0; strcpy(fp, d + 1); } else fp[0] = 0;
        strcpy(ip, digits);
        if (strcmp(ip, "0") == 0 && after > 0 && before > 0) { /* keep 0 */ }
        if (strcmp(ip, "0") == 0 && before == 0) ip[0] = 0;
        il = (int)strlen(ip);
        o = 0;
        {
            char body[96]; int b = 0;
            for (j = 0; j < il; j++) {
                body[b++] = ip[j];
                if (comma && (il - j - 1) % 3 == 0 && j != il - 1) body[b++] = ',';
            }
            body[b] = 0;
            /* sign / dollar prefix */
            {
                char pre[4]; int pl = 0;
                if (lead_plus) pre[pl++] = neg ? '-' : '+';
                else if (neg && !trail) pre[pl++] = '-';
                if (dollar) pre[pl++] = '$';
                pre[pl] = 0;
                w = before + (lead_plus ? 1 : 0);
                {
                    int need = b + pl;
                    int pad = w - need;
                    if (pad < 0) { out[o++] = '%'; pad = 0; }
                    for (j = 0; j < pad; j++) out[o++] = stars ? '*' : ' ';
                    for (j = 0; j < pl; j++) out[o++] = pre[j];
                    for (j = 0; j < b; j++) out[o++] = body[j];
                }
            }
            if (hasdot) { out[o++] = '.'; for (j = 0; fp[j]; j++) out[o++] = fp[j]; }
            if (trail == '+') out[o++] = neg ? '-' : '+';
            else if (trail == '-') out[o++] = neg ? '-' : ' ';
            out[o] = 0;
        }
    }
    out_str(out, o);
}

static void print_using(void)
{
    VAL fv;
    char fmt[256];
    int n, i, used_any;
    eval_str(&fv);
    n = fv.u.s.len;
    memcpy(fmt, fv.u.s.p, n);
    expect(';');
    i = 0;
    used_any = 0;
    for (;;) {
        VAL v;
        int got_field = 0;
        if (at_stmt_end()) break;
        eval(&v);
        /* emit literal text until a field, then format v */
        for (;;) {
            if (i >= n) {
                if (!got_field && !used_any && n > 0) { i = 0; }
                if (i >= n) { i = 0; if (!used_any) berror(E_ILLEGAL_FN); }
            }
            if (fmt[i] == '_' && i + 1 < n) { out_char((u8)fmt[i + 1]); i += 2; continue; }
            if (v.type == T_STR) {
                if (fmt[i] == '!') { out_char(v.u.s.len ? (u8)v.u.s.p[0] : ' '); i++; got_field = 1; break; }
                if (fmt[i] == '&') { out_str(v.u.s.p, v.u.s.len); i++; got_field = 1; break; }
                if (fmt[i] == '\\') {
                    int j = i + 1, w;
                    while (j < n && fmt[j] == ' ') j++;
                    if (j < n && fmt[j] == '\\') {
                        unsigned k;
                        w = j - i + 1;
                        for (k = 0; k < (unsigned)w; k++) out_char(k < v.u.s.len ? (u8)v.u.s.p[k] : ' ');
                        i = j + 1; got_field = 1; break;
                    }
                }
                if (is_numfmt_start(fmt, i, n)) berror(E_TYPE);
            } else {
                if (is_numfmt_start(fmt, i, n)) { fmt_number(fmt, &i, n, val_to_dbl(&v)); got_field = 1; break; }
                if (fmt[i] == '!' || fmt[i] == '&') berror(E_TYPE);
            }
            out_char((u8)fmt[i]);
            i++;
            if (i >= n && !used_any && !got_field) berror(E_ILLEGAL_FN);
        }
        used_any = 1;
        if (accept(';') || accept(',')) {
            if (at_stmt_end()) {
                /* trailing literal then no newline */
                return;
            }
            continue;
        }
        break;
    }
    /* flush trailing literals */
    while (i < n && !is_numfmt_start(fmt, i, n) && fmt[i] != '!' && fmt[i] != '&' && fmt[i] != '\\') {
        if (fmt[i] == '_' && i + 1 < n) { out_char((u8)fmt[i + 1]); i += 2; continue; }
        out_char((u8)fmt[i]); i++;
    }
    newline();
}

static void print_body(FILE *fp)
{
    int need_nl = 1;
    out_fp = fp;
    if (fp == 0 && accept('#')) {
        int n = eval_int();
        out_fp = file_get(n, 'O');
        if (!accept(',')) { if (!at_stmt_end()) syntax(); newline(); out_fp = 0; return; }
    }
    for (;;) {
        u8 c = peekc();
        if (c == 0 || c == ':' || c == T_ELSE) break;
        if (c == T_USING) { txtptr++; print_using(); out_fp = 0; return; }
        if (c == ';') { txtptr++; need_nl = 0; continue; }
        if (c == ',') {
            int cc = col(), nx = (cc / 14 + 1) * 14;
            txtptr++;
            if (nx + 14 > width() && !out_fp) newline();
            else while (col() < nx) out_char(' ');
            need_nl = 0;
            continue;
        }
        if (c == T_TAB || c == T_SPC) {
            int n;
            txtptr++;
            expect('('); n = eval_int(); expect(')');
            if (n < 0 || n > 255) berror(E_ILLEGAL_FN);
            if (c == T_TAB) {
                if (!out_fp) n %= scr_w;
                if (col() > n) newline();
                while (col() < n) out_char(' ');
            } else {
                while (n-- > 0) out_char(' ');
            }
            need_nl = 0;
            if (peekc() == ';') { txtptr++; }
            continue;
        }
        {
            VAL v;
            eval(&v);
            if (v.type == T_STR) out_str(v.u.s.p, v.u.s.len);
            else {
                char b[40]; int l;
                num_to_str(&v, b);
                l = (int)strlen(b);
                strcat(b, " ");
                l++;
                if (col() + l > width() && col() > 0 && !out_fp) newline();
                out_str(b, l);
            }
            need_nl = 1;
        }
    }
    if (need_nl) newline();
    out_fp = 0;
}

/* PRINT / PRINT# / LPRINT. Screen output is charged its modelled
 * per-character work, once for the whole statement; file and printer
 * output never count. */
void stmt_print(FILE *fp)
{
    con_chars = 0;
    print_body(fp);
    if (con_chars) vessel_charge(con_chars * PRINT_CHAR_CYCLES);
}
