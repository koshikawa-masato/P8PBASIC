/* token.c - tokenizer / detokenizer */
#include "basic.h"
#include <ctype.h>

typedef struct { const char *name; u8 tok; u8 func; } KW;

static const KW kwtab[] = {
 {"END",T_END,0},{"FOR",T_FOR,0},{"NEXT",T_NEXT,0},{"DATA",T_DATA,0},
 {"INPUT",T_INPUT,0},{"DIM",T_DIM,0},{"READ",T_READ,0},{"LET",T_LET,0},
 {"GOTO",T_GOTO,0},{"RUN",T_RUN,0},{"IF",T_IF,0},{"RESTORE",T_RESTORE,0},
 {"GOSUB",T_GOSUB,0},{"RETURN",T_RETURN,0},{"REM",T_REM,0},{"STOP",T_STOP,0},
 {"PRINT",T_PRINT,0},{"CLEAR",T_CLEAR,0},{"LIST",T_LIST,0},{"NEW",T_NEW,0},
 {"ON",T_ON,0},{"WAIT",T_WAIT,0},{"DEF",T_DEF,0},{"POKE",T_POKE,0},
 {"CONT",T_CONT,0},{"OUT",T_OUT,0},{"LPRINT",T_LPRINT,0},{"LLIST",T_LLIST,0},
 {"CLS",T_CLS,0},{"WIDTH",T_WIDTH,0},{"ELSE",T_ELSE,0},{"TRON",T_TRON,0},
 {"TROFF",T_TROFF,0},{"SWAP",T_SWAP,0},{"ERASE",T_ERASE,0},{"ERROR",T_ERROR,0},
 {"RESUME",T_RESUME,0},{"DELETE",T_DELETE,0},{"AUTO",T_AUTO,0},{"RENUM",T_RENUM,0},
 {"DEFSTR",T_DEFSTR,0},{"DEFINT",T_DEFINT,0},{"DEFSNG",T_DEFSNG,0},{"DEFDBL",T_DEFDBL,0},
 {"LINE",T_LINE,0},{"WHILE",T_WHILE,0},{"WEND",T_WEND,0},{"CALL",T_CALL,0},
 {"WRITE",T_WRITE,0},{"COMMON",T_COMMON,0},{"CHAIN",T_CHAIN,0},{"OPTION",T_OPTION,0},
 {"RANDOMIZE",T_RANDOMIZE,0},{"SYSTEM",T_SYSTEM,0},{"OPEN",T_OPEN,0},{"FIELD",T_FIELD,0},
 {"GET",T_GET,0},{"PUT",T_PUT,0},{"CLOSE",T_CLOSE,0},{"LOAD",T_LOAD,0},
 {"MERGE",T_MERGE,0},{"FILES",T_FILES,0},{"NAME",T_NAME,0},{"KILL",T_KILL,0},
 {"LSET",T_LSET,0},{"RSET",T_RSET,0},{"SAVE",T_SAVE,0},{"LOCATE",T_LOCATE,0},
 {"SCREEN",T_SCREEN,0},{"COLOR",T_COLOR,0},{"PSET",T_PSET,0},{"PRESET",T_PRESET,0},
 {"CIRCLE",T_CIRCLE,0},{"PAINT",T_PAINT,0},{"VIEW",T_VIEW,0},{"WINDOW",T_WINDOW,0},
 {"CONSOLE",T_CONSOLE,0},{"KEY",T_KEY,0},{"BEEP",T_BEEP,0},{"SOUND",T_SOUND,0},
 {"BLOAD",T_BLOAD,0},{"BSAVE",T_BSAVE,0},{"EDIT",T_EDIT,0},{"TO",T_TO,0},
 {"XDD",T_XDD,0},{"XDE",T_XDE,0},{"XDF",T_XDF,0},
 {"POLL",T_POLL,0},
 {"THEN",T_THEN,0},{"TAB",T_TAB,0},{"STEP",T_STEP,0},{"USR",T_USR,0},
 {"FN",T_FN,0},{"SPC",T_SPC,0},{"NOT",T_NOT,0},{"ERL",T_ERL,0},
 {"ERR",T_ERR,0},{"USING",T_USING,0},{"AND",T_AND,0},
 {"OR",T_OR,0},{"XOR",T_XOR,0},{"EQV",T_EQV,0},{"IMP",T_IMP,0},
 {"MOD",T_MOD,0},{"OFF",T_OFF,0},{"BASE",T_BASE,0},{"AS",T_AS,0},
 {"OUTPUT",T_OUTPUT,0},{"APPEND",T_APPEND,0},{"SEG",T_SEG,0},{"ALL",T_ALL,0},
 {"MID$",T_MID,0},{"TIME$",T_TIME,0},{"DATE$",T_DATE,0},{"PLAY",T_PLAY,0},
 {"ABS",F_ABS,1},{"ASC",F_ASC,1},{"ATN",F_ATN,1},{"CDBL",F_CDBL,1},
 {"CHR$",F_CHR,1},{"CINT",F_CINT,1},{"COS",F_COS,1},{"CSNG",F_CSNG,1},
 {"CVD",F_CVD,1},{"CVI",F_CVI,1},{"CVS",F_CVS,1},{"EOF",F_EOF,1},
 {"EXP",F_EXP,1},{"FIX",F_FIX,1},{"FRE",F_FRE,1},{"HEX$",F_HEX,1},
 {"INP",F_INP,1},{"INPUT$",F_INPUTS,1},{"INT",F_INT,1},{"LEFT$",F_LEFT,1},
 {"LEN",F_LEN,1},{"LOC",F_LOC,1},{"LOF",F_LOF,1},{"LOG",F_LOG,1},
 {"MKD$",F_MKD,1},{"MKI$",F_MKI,1},{"MKS$",F_MKS,1},{"OCT$",F_OCT,1},
 {"PEEK",F_PEEK,1},{"POS",F_POS,1},{"RIGHT$",F_RIGHT,1},{"RND",F_RND,1},
 {"SGN",F_SGN,1},{"SIN",F_SIN,1},{"SPACE$",F_SPACE,1},{"SQR",F_SQR,1},
 {"STR$",F_STR,1},{"TAN",F_TAN,1},{"VAL",F_VAL,1},{"INKEY$",F_INKEY,1},
 {"CSRLIN",F_CSRLIN,1},{"POINT",F_POINT,1},{"INSTR",F_INSTR,1},{"STRING$",F_STRING,1},
 {"VARPTR",F_VARPTR,1},{"ATTR$",F_ATTR,1},{"KTYPE",F_KTYPE,1},{"DSKF",F_DSKF,1},
 {"STICK",F_STICK,1},{"STRIG",F_STRIG,1},{"LPOS",F_LPOS,1},
 {"KLEN",F_KLEN,1},{"KMID$",F_KMID,1},{"KINSTR",F_KINSTR,1},
 {"KNJ$",F_KNJ,1},{"JIS$",F_JIS,1},
 {0,0,0}
};

const char *keyword_name(int tok, int func)
{
    const KW *k;
    for (k = kwtab; k->name; k++)
        if (k->tok == tok && k->func == func) return k->name;
    return "?";
}

/* longest keyword match at s; returns length or 0 */
static int match_kw(const char *s, const KW **out)
{
    const KW *k, *best = 0;
    int bl = 0;
    for (k = kwtab; k->name; k++) {
        int n = 0;
        while (k->name[n] && toupper((u8)s[n]) == (u8)k->name[n]) n++;
        if (k->name[n] == 0 && n > bl) { bl = n; best = k; }
    }
    *out = best;
    return bl;
}

static const KW *find_exact(const char *w, int len)
{
    const KW *k;
    for (k = kwtab; k->name; k++) {
        int j;
        if ((int)strlen(k->name) != len) continue;
        for (j = 0; j < len; j++) if (toupper((u8)w[j]) != (u8)k->name[j]) break;
        if (j == len) return k;
    }
    return 0;
}

static int is_sjis_lead(u8 c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }

/* keywords that may be recognized inside a running identifier */
static int mid_ident_kw(u8 t)
{
    return t == T_THEN || t == T_ELSE || t == T_TO || t == T_STEP ||
           t == T_GOTO || t == T_GOSUB;
}

#define PUT(b) do { if (o >= dstmax - 1) berror(E_LINE_OVF); dst[o++] = (u8)(b); mark = o; } while (0)
#define PUTSP() do { if (o >= dstmax - 1) berror(E_LINE_OVF); dst[o++] = ' '; } while (0)

/* tokenize src -> dst. returns length (terminating 0 included) */
int tokenize(const char *src, u8 *dst, int dstmax)
{
    const u8 *s = (const u8 *)src;
    int o = 0, mark = 0;
    int expect_line = 0;   /* after GOTO/GOSUB/THEN/ELSE/RESTORE/RUN etc */
    int in_ident = 0;
    int stmt_head = 1;     /* at the start of a statement (label definitions) */
    while (*s) {
        u8 c = *s;
        int head = stmt_head;
        if (c != ' ') stmt_head = (c == ':');
        /* labels: "*NAME" at statement start, or as a jump target */
        if (c == '*' && isalpha(s[1]) && (head || expect_line)) {
            PUT('*'); s++;
            while (isalnum(*s) || *s == '.') PUT(toupper(*s++));
            in_ident = 0;
            continue;
        }
        if (c == '"') {
            PUT(*s++);
            while (*s && *s != '"') PUT(*s++);
            if (*s == '"') PUT(*s++);
            in_ident = 0; expect_line = 0;
            continue;
        }
        if (c == '\'') {
            PUT(':'); PUT(T_REM); PUT(T_APOS);
            s++;
            while (*s) PUT(*s++);
            break;
        }
        if (c == '?' ) { PUT(T_PRINT); s++; in_ident = 0; expect_line = 0; continue; }
        if (c == ' ' || c < 0x20) { PUTSP(); s++; in_ident = 0; continue; }  /* control codes outside strings -> space */
        if (c >= 0x80) {          /* stray kana/kanji outside strings */
            PUT(*s++);
            if (is_sjis_lead(c) && *s) PUT(*s++);
            continue;
        }
        if (isalpha(c)) {
            /* N88 style: read the whole word first.  A word is a keyword only when
             * the whole word (optionally with '$') is a keyword, or when a keyword is
             * directly followed by digits (GOTO100, TO9).  Otherwise it is a name,
             * so variables such as OUTE or STOC are kept intact. */
            const KW *k = 0;
            int wl = 0, n = 0;
            while (isalnum(s[wl]) || s[wl] == '.') wl++;
            if (s[wl] == '$') { k = find_exact((const char *)s, wl + 1); if (k) n = wl + 1; }
            if (!n) { k = find_exact((const char *)s, wl); if (k) n = wl; }
            if (!n && wl == 2 && toupper(s[0]) == 'G' && toupper(s[1]) == 'O') {
                /* GO TO / GO SUB */
                const u8 *q = s + 2; int sp2 = 0;
                while (q[sp2] == ' ') sp2++;
                if (sp2 && toupper(q[sp2]) == 'T' && toupper(q[sp2 + 1]) == 'O' && !isalpha(q[sp2 + 2])) {
                    k = find_exact("GOTO", 4); n = 2 + sp2 + 2;
                } else if (sp2 && toupper(q[sp2]) == 'S' && toupper(q[sp2 + 1]) == 'U' && toupper(q[sp2 + 2]) == 'B' && !isalpha(q[sp2 + 3])) {
                    k = find_exact("GOSUB", 5); n = 2 + sp2 + 3;
                }
            }
            if (!n && wl > 2 && toupper(s[0]) == 'F' && toupper(s[1]) == 'N') {
                k = find_exact("FN", 2); n = 2;
            }
            if (!n) {
                /* keyword immediately followed by a digit */
                const KW *kk; int best = 0;
                for (kk = kwtab; kk->name; kk++) {
                    int L = (int)strlen(kk->name), j;
                    if (L >= wl || L <= best || kk->name[L - 1] == '$') continue;
                    if (!isdigit(s[L])) continue;
                    for (j = 0; j < L; j++) if (toupper(s[j]) != (u8)kk->name[j]) break;
                    if (j == L) { best = L; k = kk; }
                }
                n = best;
            }
            if (n) {
                if (k->func) { PUT(TK_FUNC); PUT(k->tok); }
                else PUT(k->tok);
                s += n;
                in_ident = 0;
                if (!k->func && (k->tok == T_REM)) {
                    while (*s) PUT(*s++);
                    break;
                }
                if (!k->func && k->tok == T_DATA) {
                    int q = 0;
                    while (*s && (q || *s != ':')) { if (*s == '"') q = !q; PUT(*s++); }
                    continue;
                }
                if (!k->func && (k->tok == T_FN)) {
                    /* function name follows: copy identifier verbatim */
                    while (*s == ' ') PUT(*s++);
                    while (isalnum(*s) || *s == '.') PUT(toupper(*s++));
                    if (*s == '%' || *s == '!' || *s == '#' || *s == '$') PUT(*s++);
                    continue;
                }
                expect_line = (!k->func && (k->tok == T_GOTO || k->tok == T_GOSUB ||
                    k->tok == T_THEN || k->tok == T_ELSE || k->tok == T_RESTORE ||
                    k->tok == T_RUN || k->tok == T_RESUME || k->tok == T_LIST ||
                    k->tok == T_DELETE || k->tok == T_EDIT || k->tok == T_LLIST ||
                    k->tok == T_RENUM || k->tok == T_AUTO));
                continue;
            }
            /* a name */
            {
                int j;
                for (j = 0; j < wl; j++) PUT(toupper(s[j]));
                s += wl;
                if (*s == '%' || *s == '!' || *s == '#' || *s == '$') PUT(*s++);
                in_ident = 0;
                expect_line = 0;
            }
            continue;
        }
        if (in_ident && (isdigit(c) || c == '.')) {
            PUT(c); s++;
            if (*s == '%' || *s == '!' || *s == '#' || *s == '$') { PUT(*s++); in_ident = 0; }
            continue;
        }
        in_ident = 0;
        if (c == '&' && (s[1] == 'H' || s[1] == 'h' || s[1] == 'O' || s[1] == 'o' || isdigit(s[1]))) {
            long v = 0; int hex = (s[1] == 'H' || s[1] == 'h');
            const u8 *p = s + 1;
            if (!isdigit(s[1])) p++;
            if (hex) { while (isxdigit(*p)) { v = v * 16 + (isdigit(*p) ? *p - '0' : (toupper(*p) - 'A' + 10)); p++; if (v > 0xFFFFL) berror(E_OVERFLOW); } }
            else { while (*p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; if (v > 0xFFFFL) berror(E_OVERFLOW); } }
            PUT(hex ? TK_HEX : TK_OCT); PUT(v & 0xFF); PUT((v >> 8) & 0xFF);
            s = p;
            expect_line = 0;
            continue;
        }
        if (isdigit(c) || (c == '.' && isdigit(s[1]))) {
            if (expect_line && c != '.') {
                long v = 0;
                while (isdigit(*s)) { v = v * 10 + (*s - '0'); s++; if (v > 65529L) berror(E_SYNTAX); }
                PUT(TK_LINENUM); PUT(v & 0xFF); PUT((v >> 8) & 0xFF);
                continue;
            } else {
                VAL v; int used;
                str_to_num((const char *)s, &v, &used);
                s += used;
                if (v.type == T_INT) { PUT(TK_INT); PUT(v.u.i & 0xFF); PUT((v.u.i >> 8) & 0xFF); }
                else if (v.type == T_SNG) { u8 *b = (u8 *)&v.u.f; int i; PUT(TK_SNG); for (i = 0; i < 4; i++) PUT(b[i]); }
                else { u8 *b = (u8 *)&v.u.d; int i; PUT(TK_DBL); for (i = 0; i < 8; i++) PUT(b[i]); }
                continue;
            }
        }
        if (c == ',') { PUT(c); s++; continue; }   /* keep expect_line for ON..GOTO lists */
        expect_line = 0;
        PUT(c); s++;
    }
    /* trim trailing spaces (only real spaces, never bytes inside a constant) */
    o = mark;
    dst[o++] = 0;
    return o;
}

/* ---------------- detokenize ---------------- */
void detokenize_line(const u8 *t, char *out, int outmax)
{
    int o = 0;
    int inq = 0, raw = 0;
    char buf[40];
#define OUTC(ch) do { if (o < outmax - 1) out[o++] = (ch); } while (0)
    while (*t) {
        u8 c = *t++;
        if (raw) { OUTC(c); continue; }
        if (inq) { OUTC(c); if (c == '"') inq = 0; continue; }
        if (c == '"') { inq = 1; OUTC(c); continue; }
        if (c == ':' && t[0] == T_REM && t[1] == T_APOS) {
            OUTC('\''); t += 2; raw = 1; continue;
        }
        if (c == TK_FUNC) {
            const char *n = keyword_name(*t++, 1);
            while (*n) OUTC(*n++);
            continue;
        }
        if (c == TK_LINENUM || c == TK_INT) {
            u16 v = t[0] | (t[1] << 8); t += 2;
            if (c == TK_INT) sprintf(buf, "%d", (i16)v); else sprintf(buf, "%u", v);
            { char *p = buf; while (*p) OUTC(*p++); }
            continue;
        }
        if (c == TK_HEX || c == TK_OCT) {
            u16 v = t[0] | (t[1] << 8); t += 2;
            if (c == TK_HEX) sprintf(buf, "&H%X", v); else sprintf(buf, "&O%o", v);
            { char *p = buf; while (*p) OUTC(*p++); }
            continue;
        }
        if (c == TK_SNG || c == TK_DBL) {
            VAL v; char *p;
            if (c == TK_SNG) { v.type = T_SNG; memcpy(&v.u.f, t, 4); t += 4; }
            else { v.type = T_DBL; memcpy(&v.u.d, t, 8); t += 8; }
            num_to_str(&v, buf);
            p = buf; if (*p == ' ') p++;
            while (*p) { if (*p != ' ') OUTC(*p); p++; }
            /* type suffix where needed for re-tokenize fidelity */
            if (v.type == T_DBL && !strchr(buf, 'D')) {
                /* doubles which print with <= 7 digits need '#' */
                int digits = 0; char *q;
                for (q = buf; *q; q++) if (*q >= '0' && *q <= '9') digits++;
                if (digits <= 7) OUTC('#');
            }
            if (v.type == T_SNG && strchr(buf, '.') == 0 && strchr(buf, 'E') == 0) {
                /* integer-valued single beyond int range e.g. 40000 prints as is */
                long lv = (long)v.u.f;
                if (lv >= -32768L && lv <= 32767L) OUTC('!');
            }
            continue;
        }
        if (c >= 0x80 && c < T_LAST_STMT) {
            const char *n = keyword_name(c, 0);
            while (*n) OUTC(*n++);
            if (c == T_REM || c == T_DATA) {
                if (c == T_REM) raw = 1;
                else {
                    int q = 0;
                    while (*t && (q || *t != ':')) { if (*t == '"') q = !q; OUTC(*t); t++; }
                }
            }
            continue;
        }
        OUTC(c);
    }
    out[o] = 0;
#undef OUTC
}
