/* screen.c - text console and N88 style full screen line editor */
#include "basic.h"

int scr_w = 80, scr_h = 25, cur_x = 0, cur_y = 0, con_top = 0, con_bot = 23, key_disp = 1;
unsigned char text_attr = 0xE1;
int con_mono = 0;

static u16 cell[25][80];
static u8 cattr[25][80];
static u8 cont[25];
static int pend_wrap = 0;
static int sjis_lead = 0;
static int ins_mode = 0;
static int cursor_on_prog = 0;  /* LOCATE ,,1 */
int ank_mode = 0;               /* /G : treat 80h-9Fh,E0h-FFh as 1-byte graphic characters */

#define FKEY_DEFAULTS { \
    "list ", "run\r", "load \"", "save \"", "cont\r", \
    "files\r", "auto ", "renum ", "key ", "screen " \
}
static const char fkey_default[10][16] = FKEY_DEFAULTS;
char fkey_str[10][16] = FKEY_DEFAULTS;
static void input_state_reset(void);    /* escape parser + pending keys, defined below */

/* key trap hooks in exec.c */
void key_trap_signal(int n);
int key_trap_enabled(int n);

#define BLANK_ATTR(a) ((u8)((a) & 0xE1))
#define PAD 0xFFFEu                          /* filler cell before a kanji that did not fit */
#define IS_RIGHT(c) (((c) & 0x8000) && (c) != PAD)

/* ---------------- low level ---------------- */
static int last_row(void) { return con_bot; }

static void put_cell(int x, int y, u16 code, u8 a)
{
    u16 v;
    cell[y][x] = code; cattr[y][x] = a;
    if (code == PAD) v = ' ';
    else if (code < 0x100) v = code;
    else if (code & 0x8000) {           /* right half of kanji */
        u16 j = code & 0x7FFF;
        v = (u16)((((j & 0xFF)) << 8) | (((j >> 8) - 0x20) | 0x80));
    } else {
        v = (u16)(((code & 0xFF) << 8) | ((code >> 8) - 0x20));
    }
    hal_tput(x, y, v, a);
}

static void redraw_row(int y)
{
    static u16 vbuf[80];
    int x;
    for (x = 0; x < scr_w; x++) {
        u16 code = cell[y][x], v;
        if (code == PAD) v = ' ';
        else if (code < 0x100) v = code;
        else if (code & 0x8000) { u16 j = code & 0x7FFF; v = (u16)(((j & 0xFF) << 8) | (((j >> 8) - 0x20) | 0x80)); }
        else v = (u16)(((code & 0xFF) << 8) | ((code >> 8) - 0x20));
        vbuf[x] = v;
    }
    hal_trow(y, vbuf, cattr[y], scr_w);
}

static void clear_row(int y)
{
    int x;
    for (x = 0; x < scr_w; x++) { cell[y][x] = ' '; cattr[y][x] = BLANK_ATTR(text_attr); }
    cont[y] = 0;
    redraw_row(y);
}

/* scroll region [top,bot] up by one */
static void scroll_up_region(int top, int bot)
{
    int y;
    for (y = top; y < bot; y++) {
        memcpy(cell[y], cell[y + 1], sizeof(cell[0]));
        memcpy(cattr[y], cattr[y + 1], sizeof(cattr[0]));
        cont[y] = cont[y + 1];
        redraw_row(y);
    }
    if (top <= bot) cont[top] = 0;
    clear_row(bot);
}

/* scroll region [top,bot] down by one (insert blank row at top) */
static void scroll_down_region(int top, int bot)
{
    int y;
    for (y = bot; y > top; y--) {
        memcpy(cell[y], cell[y - 1], sizeof(cell[0]));
        memcpy(cattr[y], cattr[y - 1], sizeof(cattr[0]));
        cont[y] = cont[y - 1];
        redraw_row(y);
    }
    clear_row(top);
}

void con_scroll_up(void) { scroll_up_region(con_top, last_row()); }

void con_key_redraw(void)
{
    int i, y = scr_h - 1;
    if (!key_disp) return;
    for (i = 0; i < scr_w; i++) { cell[y][i] = ' '; cattr[y][i] = 0xE1; }
    for (i = 0; i < 10; i++) {
        int x0, j, w = (scr_w == 80) ? 6 : 3;
        int gap = (scr_w == 80) ? 7 : 4;
        x0 = i * gap + (i >= 5 ? (scr_w == 80 ? 5 : 0) : 0);
        if (x0 + w > scr_w) break;
        for (j = 0; j < w; j++) {
            u8 c = (u8)fkey_str[i][j];
            int k;
            for (k = 0; k < j; k++) if (fkey_str[i][k] == 0) { c = 0; break; }
            if (c < 0x20) c = ' ';
            cell[y][x0 + j] = c;
            cattr[y][x0 + j] = 0xE5;   /* white reverse */
        }
    }
    cont[y] = 0;
    redraw_row(y);
}

void con_keydisp(int on)
{
    key_disp = on ? 1 : 0;
    if (key_disp) {
        if (con_bot > scr_h - 2) con_bot = scr_h - 2;
        if (cur_y > con_bot) cur_y = con_bot;
        con_key_redraw();
    } else {
        int y = scr_h - 1;
        if (con_bot == scr_h - 2) con_bot = scr_h - 1;
        clear_row(y);
    }
}

void scr_cls(void)
{
    int y;
    for (y = con_top; y <= con_bot; y++) clear_row(y);
    /* one text memory on hardware: image-resident text goes too */
    hal_text_clear_region(con_top, con_bot);
    cur_x = 0; cur_y = con_top; pend_wrap = 0;
}

/* Runs once per session. A process can host several sessions in
 * turn (engine.c), so everything a program can change is put back
 * to its power-on value here. ank_mode is a host option (/G, set
 * before this call) and is kept. */
void scr_init(void)
{
    int y;
    scr_w = 80; scr_h = 25;             /* before con_bot is derived */
    text_attr = 0xE1; con_mono = 0;
    memcpy(fkey_str, fkey_default, sizeof(fkey_str));
    sjis_lead = 0; ins_mode = 0; cursor_on_prog = 0;
    cur_x = 0; cur_y = 0;
    input_state_reset();
    for (y = 0; y < 25; y++) clear_row(y);
    con_top = 0; con_bot = scr_h - 1;
    con_keydisp(1);
    scr_cls();
}

void con_set_width(int w, int h)
{
    int y;
    hal_width40(w == 40);
    /* clear everything */
    for (y = 0; y < 25; y++) {
        int x;
        for (x = 0; x < 80; x++) { cell[y][x] = ' '; cattr[y][x] = 0xE1; }
        cont[y] = 0;
    }
    scr_w = w; scr_h = h;
    {
        static u16 sp[80]; static u8 at[80];
        int x; for (x = 0; x < 80; x++) { sp[x] = ' '; at[x] = 0xE1; }
        hal_width40(0);
        for (y = 0; y < 25; y++) hal_trow(y, sp, at, 80);
        hal_width40(w == 40);
    }
    con_top = 0; con_bot = scr_h - 1;
    con_keydisp(key_disp);
    scr_cls();
}

void con_console(int top, int lines, int keysw)
{
    con_top = top;
    con_bot = top + lines - 1;
    if (con_bot > scr_h - 1) con_bot = scr_h - 1;
    key_disp = keysw ? 1 : 0;
    if (key_disp && con_bot > scr_h - 2) con_bot = scr_h - 2;
    if (!key_disp) clear_row(scr_h - 1); else con_key_redraw();
    cur_x = 0; cur_y = con_top; pend_wrap = 0;
}

void con_locate(int x, int y)
{
    if (x >= scr_w) x = scr_w - 1;
    if (y > scr_h - 1) y = scr_h - 1;
    cur_x = x; cur_y = y; pend_wrap = 0;
}

void con_newline(void)
{
    pend_wrap = 0;
    cur_x = 0;
    if (cur_y >= con_bot) { scroll_up_region(con_top, con_bot); cur_y = con_bot; }
    else cur_y++;
    cont[cur_y] = 0;
}

static void advance_wrap(void)
{
    /* move to next row as continuation */
    cur_x = 0;
    if (cur_y >= con_bot) { scroll_up_region(con_top, con_bot); cur_y = con_bot; }
    else cur_y++;
    cont[cur_y] = 1;
}

static void put_printable(u16 code, int wide)
{
    if (pend_wrap) { pend_wrap = 0; advance_wrap(); }
    if (wide && cur_x >= scr_w - 1) {
        put_cell(cur_x, cur_y, PAD, text_attr);
        advance_wrap();
    }
    if (wide) {
        put_cell(cur_x, cur_y, code, text_attr);
        put_cell(cur_x + 1, cur_y, (u16)(code | 0x8000), text_attr);
        cur_x += 2;
    } else {
        put_cell(cur_x, cur_y, code, text_attr);
        cur_x++;
    }
    if (cur_x >= scr_w) { cur_x = scr_w - 1; pend_wrap = 1; }
}

static unsigned sjis_to_jis(unsigned c1, unsigned c2)
{
    c1 -= (c1 <= 0x9F) ? 0x71 : 0xB1;
    c1 = c1 * 2 + 1;
    if (c2 > 0x7F) c2--;
    if (c2 >= 0x9E) { c2 -= 0x7D; c1++; } else c2 -= 0x1F;
    return (c1 << 8) | c2;
}

static unsigned jis_to_sjis(unsigned jis)
{
    unsigned c1 = (jis >> 8) & 0x7F, c2 = jis & 0x7F, s1, s2;
    s1 = ((c1 + 1) >> 1) + (c1 <= 0x5E ? 0x70 : 0xB0);
    s2 = c2 + ((c1 & 1) ? (c2 >= 0x60 ? 0x20 : 0x1F) : 0x7E);
    return (s1 << 8) | s2;
}

static int is_lead(int c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }

/* ---- escape sequences ---- */
static int esc_state = 0;
static char esc_buf[16];
static int esc_len = 0;

static void ansi_color(int n)
{
    static const u8 map[8] = {0, 2, 4, 6, 1, 3, 5, 7};   /* ANSI -> PC98 GRB */
    if (n == 0) text_attr = 0xE1;
    else if (n >= 30 && n <= 37) text_attr = (u8)((text_attr & 0x1F) | (map[n - 30] << 5));
    else if (n >= 16 && n <= 23) text_attr = (u8)((text_attr & 0x1F) | ((n - 16) << 5));
    else if (n == 7) text_attr |= 0x04;
    else if (n == 5) text_attr |= 0x02;
    else if (n == 4) text_attr |= 0x08;
    else if (n == 8) text_attr &= (u8)~1;
}

static void esc_finish(int final)
{
    int p[4] = {0, 0, 0, 0}, np = 0, i = 0, priv = 0;
    if (esc_buf[0] == '>') { priv = 1; i = 1; }
    for (; i < esc_len && np < 4; i++) {
        if (esc_buf[i] >= '0' && esc_buf[i] <= '9') p[np] = p[np] * 10 + (esc_buf[i] - '0');
        else if (esc_buf[i] == ';') np++;
    }
    np++;
    if (priv) {
        if (final == 'h' && p[0] == 5) cursor_on_prog = 0;
        else if (final == 'l' && p[0] == 5) cursor_on_prog = 1;
        else if (final == 'h' && p[0] == 1) con_keydisp(0);
        else if (final == 'l' && p[0] == 1) con_keydisp(1);
        return;
    }
    switch (final) {
    case 'A': cur_y -= p[0] ? p[0] : 1; if (cur_y < con_top) cur_y = con_top; break;
    case 'B': cur_y += p[0] ? p[0] : 1; if (cur_y > con_bot) cur_y = con_bot; break;
    case 'C': cur_x += p[0] ? p[0] : 1; if (cur_x >= scr_w) cur_x = scr_w - 1; break;
    case 'D': cur_x -= p[0] ? p[0] : 1; if (cur_x < 0) cur_x = 0; break;
    case 'H': case 'f':
        cur_y = (p[0] ? p[0] : 1) - 1; cur_x = (p[1] ? p[1] : 1) - 1;
        if (cur_y > scr_h - 1) cur_y = scr_h - 1;
        if (cur_x > scr_w - 1) cur_x = scr_w - 1;
        break;
    case 'J': if (p[0] == 2) scr_cls(); break;
    case 'K': { int x; for (x = cur_x; x < scr_w; x++) put_cell(x, cur_y, ' ', BLANK_ATTR(text_attr)); } break;
    case 'm': for (i = 0; i < np; i++) ansi_color(p[i]); break;
    }
    pend_wrap = 0;
}

void con_putc(int c)
{
    c &= 0xFF;
    if (esc_state == 1) {
        if (c == '[') { esc_state = 2; esc_len = 0; return; }
        esc_state = 0;
        if (c == '*') { scr_cls(); return; }
        return;
    }
    if (esc_state == 2) {
        if ((c >= '0' && c <= '9') || c == ';' || c == '>') { if (esc_len < 15) esc_buf[esc_len++] = (char)c; return; }
        esc_state = 0;
        esc_finish(c);
        return;
    }
    if (sjis_lead) {
        int lead = sjis_lead;
        sjis_lead = 0;
        if (c >= 0x40 && c <= 0xFC && c != 0x7F) {
            unsigned j = sjis_to_jis(lead, c);
            unsigned c1 = j >> 8, c2 = j & 0xFF;
            if (c1 >= 0x21 && c1 <= 0x7E && c2 >= 0x21 && c2 <= 0x7E) {
                put_printable((u16)j, 1);
                return;
            }
        }
        /* not a valid kanji: show the lead byte as a 1-byte character */
        put_printable((u16)lead, 0);
    }
    if (c < 0x20) {
        switch (c) {
        case 7: { unsigned long t0 = hal_ticks(); long g = 0; hal_beep(1); while (hal_ticks() - t0 < 4 && ++g < 1000000L) hal_idle(); hal_beep(0); } return;
        case 8: pend_wrap = 0; if (cur_x > 0) cur_x--; else if (cur_y > con_top) { cur_y--; cur_x = scr_w - 1; } return;
        case 9: { int n = 8 - (cur_x % 8); while (n--) put_printable(' ', 0); } return;
        case 10: case 13: con_newline(); return;
        case 11: cur_x = 0; cur_y = con_top; pend_wrap = 0; return;
        case 12: scr_cls(); return;
        case 27: esc_state = 1; return;
        case 28: pend_wrap = 0; if (cur_x < scr_w - 1) cur_x++; else if (cur_y < con_bot) { cur_x = 0; cur_y++; } return;
        case 29: pend_wrap = 0; if (cur_x > 0) cur_x--; else if (cur_y > con_top) { cur_y--; cur_x = scr_w - 1; } return;
        case 30: pend_wrap = 0; if (cur_y > con_top) cur_y--; return;
        case 31: pend_wrap = 0; if (cur_y < con_bot) cur_y++; return;
        default: put_printable((u16)c, 0); return;
        }
    }
    if (!ank_mode && is_lead(c)) { sjis_lead = c; return; }
    put_printable((u16)c, 0);
}

void con_puts(const char *s)
{
    while (*s) con_putc((u8)*s++);
}

unsigned short scr_code(int x, int y) { return cell[y][x]; }

/* ---------------- keyboard ---------------- */
static char kpend[32];
static int kpend_n = 0, kpend_i = 0;
static int kq[16]; static int kq_n = 0;   /* pushed back keys */

static void input_state_reset(void)
{
    esc_state = 0; esc_len = 0;
    kpend_n = kpend_i = 0; kq_n = 0;
}

void con_flush_keys(void)
{
    kpend_n = kpend_i = 0;
    while (hal_getkey() >= 0) {}
}

static int translate(int k)
{
    if (k >= KC_F1 && k < KC_F1 + 10) {
        int n = k - KC_F1;
        if (running && key_trap_enabled(n)) { key_trap_signal(n); return -1; }
        strcpy(kpend, fkey_str[n]);
        kpend_n = (int)strlen(kpend); kpend_i = 0;
        if (kpend_n == 0) return -1;
        return (u8)kpend[kpend_i++];
    }
    if (k >= KC_SF1 && k < KC_SF1 + 10) {
        int n = k - KC_SF1;
        if (running && key_trap_enabled(n)) { key_trap_signal(n); return -1; }
        strcpy(kpend, fkey_str[n]);
        kpend_n = (int)strlen(kpend); kpend_i = 0;
        if (kpend_n == 0) return -1;
        return (u8)kpend[kpend_i++];
    }
    if (k == KC_SHOME) return 0x0C;
    if (k == KC_HELP || k == KC_ROLLUP || k == KC_ROLLDN || k == KC_COPY) return -1;
    if (k >= 0x100) return -1;
    return k;
}

int con_getchar(int wait)
{
    for (;;) {
        int k;
        if (kq_n > 0) return kq[--kq_n];
        if (kpend_i < kpend_n) return (u8)kpend[kpend_i++];
        k = hal_getkey();
        if (k >= 0) {
            k = translate(k);
            if (k >= 0) return k;
            continue;
        }
        if (!wait) return -1;
        if (running && hal_breakflag()) return -2;
        hal_idle();
    }
}

int con_inkey(void)
{
    int c = con_getchar(0);
    return c < 0 ? -1 : c;
}

/* ---------------- line editor ---------------- */
static int lline_top(int y) { while (y > con_top && cont[y]) y--; return y; }
static int lline_bot(int y) { while (y < con_bot && cont[y + 1]) y++; return y; }

static void show_cursor(int on)
{
    hal_cursor(cur_x, cur_y, on, ins_mode);
}

/* ---- logical line editing ----
 * The logical line is taken out as a list of characters (ANK = 1 cell,
 * kanji = 2 cells), edited, and laid out again so that a kanji is never
 * split.  A kanji that does not fit at the last column leaves a PAD cell. */
typedef struct { u16 code; u8 attr; } ECH;
static ECH ebuf[25 * 80 + 8];
static int en, ecur, e_top, e_bot;
static int in_sx = 0, in_sy = 0;     /* start of INPUT text */

static int is_wide(u16 c) { return c >= 0x100 && c != PAD && !(c & 0x8000); }

static void ed_extract(void)
{
    int x, y;
    e_top = lline_top(cur_y); e_bot = lline_bot(cur_y);
    en = 0; ecur = -1;
    for (y = e_top; y <= e_bot; y++)
        for (x = 0; x < scr_w; x++) {
            u16 c = cell[y][x];
            if (y == cur_y && x == cur_x && ecur < 0) ecur = IS_RIGHT(c) ? en - 1 : en;
            if (c == PAD || IS_RIGHT(c)) continue;
            ebuf[en].code = c; ebuf[en].attr = cattr[y][x]; en++;
        }
    if (ecur < 0) ecur = en;
}

static void ed_newrow(int *y)
{
    (*y)++;
    if (*y <= e_bot) return;
    if (*y > con_bot) {
        scroll_up_region(con_top, con_bot);
        if (e_top > con_top) e_top--;
        e_bot--; (*y)--;
        in_sy--;
        clear_row(*y);
    } else {
        scroll_down_region(*y, con_bot);
    }
    cont[*y] = 1;
    e_bot = *y;
}

static void ed_render(void)
{
    int i, x = 0, y = e_top, cx = 0, cy = e_top, yy, last;
    last = en - 1;
    while (last >= 0 && ebuf[last].code == ' ') last--;
    for (i = 0; ; i++) {
        int w;
        if (i >= en || (i > last && i >= ecur)) break;   /* only trailing spaces remain */
        w = is_wide(ebuf[i].code) ? 2 : 1;
        if (x + w > scr_w) {
            if (x < scr_w) put_cell(x, y, PAD, ebuf[i].attr);
            ed_newrow(&y); x = 0;
        }
        if (i == ecur) { cx = x; cy = y; }
        put_cell(x, y, ebuf[i].code, ebuf[i].attr);
        if (w == 2) put_cell(x + 1, y, (u16)(ebuf[i].code | 0x8000), ebuf[i].attr);
        x += w;
    }
    if (ecur >= i) {
        if (x >= scr_w) { ed_newrow(&y); x = 0; }
        cx = x; cy = y;
    }
    for (; x < scr_w; x++) put_cell(x, y, ' ', BLANK_ATTR(text_attr));
    for (yy = y + 1; yy <= e_bot; yy++)
        for (x = 0; x < scr_w; x++) put_cell(x, yy, ' ', BLANK_ATTR(text_attr));
    cur_x = cx; cur_y = cy;
}

static void ed_type(u16 code)
{
    int i;
    ed_extract();
    if (ins_mode || ecur >= en) {
        for (i = en; i > ecur; i--) ebuf[i] = ebuf[i - 1];
        en++;
    }
    ebuf[ecur].code = code; ebuf[ecur].attr = text_attr;
    ecur++;
    ed_render();
}

static void ed_delete(int before)
{
    int i;
    ed_extract();
    if (before) { if (ecur == 0) return; ecur--; }
    if (ecur >= en) { ed_render(); return; }
    for (i = ecur; i < en - 1; i++) ebuf[i] = ebuf[i + 1];
    en--;
    ed_render();
}

static void ed_erase_eol(void)
{
    ed_extract();
    en = ecur;
    ed_render();
}

/* keep the cursor off the right half of a kanji and off PAD cells */
static void cursor_fix_left(void)
{
    int n = 3;
    while (n-- > 0 && cur_x > 0 && (IS_RIGHT(cell[cur_y][cur_x]) || cell[cur_y][cur_x] == PAD)) cur_x--;
}

int con_lineinput(char *buf, int max, int is_input)
{
    int c;
    pend_wrap = 0;
    if (cur_x >= scr_w) cur_x = scr_w - 1;
    in_sx = cur_x; in_sy = cur_y;
    ins_mode = 0;
    for (;;) {
        show_cursor(1);
        c = con_getchar(1);
        if (c == -2 || hal_breakflag()) {
            show_cursor(0);
            con_newline();
            ins_mode = 0;
            return -1;
        }
        if (c < 0) continue;
        if (c == 3) { show_cursor(0); con_puts("^C"); con_newline(); return -1; }
        if (c == 13) {
            int top = lline_top(cur_y), bot = lline_bot(cur_y);
            int x0 = 0, y0 = top, n = 0, x, y;
            if (is_input && in_sy >= top && in_sy <= bot) { x0 = in_sx; y0 = in_sy; }
            for (y = y0; y <= bot; y++) {
                for (x = (y == y0 ? x0 : 0); x < scr_w; x++) {
                    u16 code = cell[y][x];
                    if (code & 0x8000) continue;     /* right half or PAD */
                    if (code >= 0x100) {
                        unsigned s = jis_to_sjis(code);
                        if (n < max - 2) { buf[n++] = (char)(s >> 8); buf[n++] = (char)(s & 0xFF); }
                    } else if (n < max - 1) buf[n++] = (char)code;
                }
            }
            while (n > 0 && buf[n - 1] == ' ') n--;
            buf[n] = 0;
            show_cursor(0);
            cur_y = bot; cur_x = 0;
            con_newline();
            ins_mode = 0;
            return n;
        }
        switch (c) {
        case 0x1C:
            ins_mode = 0;
            {
                int w = is_wide(cell[cur_y][cur_x]) ? 2 : 1;
                if (cell[cur_y][cur_x] == PAD) w = scr_w;
                if (cur_x + w < scr_w) cur_x += w;
                else if (cur_y < con_bot) { cur_x = 0; cur_y++; }
            }
            continue;
        case 0x1D:
            ins_mode = 0;
            if (cur_x > 0) cur_x--; else if (cur_y > con_top) { cur_x = scr_w - 1; cur_y--; }
            cursor_fix_left();
            continue;
        case 0x1E: ins_mode = 0; if (cur_y > con_top) cur_y--; cursor_fix_left(); continue;
        case 0x1F: ins_mode = 0; if (cur_y < con_bot) cur_y++; cursor_fix_left(); continue;
        case 0x0B: ins_mode = 0; cur_x = 0; cur_y = con_top; continue;
        case 0x0C: ins_mode = 0; scr_cls(); in_sx = 0; in_sy = con_top; continue;
        case 0x12: ins_mode = !ins_mode; continue;
        case 0x7F: ed_delete(0); continue;
        case 0x08: ed_delete(1); continue;
        case 0x09: { int n = 8 - (cur_x % 8); while (n-- > 0 && cur_x < scr_w - 1) cur_x++; cursor_fix_left(); } continue;
        case 0x1B: continue;
        case 0x05: ed_erase_eol(); continue;  /* CTRL+E : erase to end of logical line */
        default: break;
        }
        if (c < 0x20) continue;
        if (!ank_mode && is_lead(c)) {
            /* Shift_JIS: the second byte follows immediately (IME / KEY strings) */
            int c2 = con_getchar(1);
            if (c2 >= 0x40 && c2 <= 0xFC && c2 != 0x7F) {
                unsigned j = sjis_to_jis((unsigned)c, (unsigned)c2);
                if ((j >> 8) >= 0x21 && (j >> 8) <= 0x7E && (j & 0xFF) >= 0x21 && (j & 0xFF) <= 0x7E) {
                    ed_type((u16)j);
                    continue;
                }
            }
            ed_type((u16)c);
            if (c2 >= 0 && kq_n < 16) kq[kq_n++] = c2;
            continue;
        }
        ed_type((u16)c);
    }
}
