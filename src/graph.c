/* graph.c - graphics statements */
#include "basic.h"
#include "vessel.h"
#include <ctype.h>
#include <math.h>

void *array_base(const char *name, int type, long *bytes);

int g_fg = 7, g_bg = 0;
static int g_mode = 3;           /* SCREEN mode */
static int g_on = 1;
static int g_apage = 0, g_dpage = 1;
static int lastx = 0, lasty = 0;
static int vx1 = 0, vy1 = 0, vx2 = 639, vy2 = 399;  /* view (logical) */
static int win_on = 0;
static double wx1, wy1, wx2, wy2;

static int lines_of(void) { return (g_mode == 0 || g_mode == 1) ? 200 : 400; }
static int mono(void) { return g_mode == 1 || g_mode == 2; }

static int mapc(int c)
{
    if (mono()) return c ? 7 : 0;
    return c & 15;
}

static void pset_raw(int x, int y, int c)
{
    if (x < vx1 || x > vx2 || y < vy1 || y > vy2) return;
    if (lines_of() == 200) { hal_gpset(x, y * 2, c); hal_gpset(x, y * 2 + 1, c); }
    else hal_gpset(x, y, c);
}

static int point_raw(int x, int y)
{
    if (x < 0 || x > 639 || y < 0 || y >= lines_of()) return -1;
    return hal_gpoint(x, lines_of() == 200 ? y * 2 : y);
}

static void hline_raw(int x1, int x2, int y, int c)
{
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y < vy1 || y > vy2) return;
    if (x1 < vx1) x1 = vx1;
    if (x2 > vx2) x2 = vx2;
    if (x1 > x2) return;
    if (lines_of() == 200) { hal_ghline(x1, x2, y * 2, c); hal_ghline(x1, x2, y * 2 + 1, c); }
    else hal_ghline(x1, x2, y, c);
}

void g_cls(void)
{
    if (vx1 == 0 && vy1 == 0 && vx2 == 639 && vy2 == lines_of() - 1) hal_gclear(mapc(g_bg));
    else { int y; for (y = vy1; y <= vy2; y++) hline_raw(vx1, vx2, y, mapc(g_bg)); }
}

static void default_palette(void)
{
    int i;
    for (i = 0; i < 8; i++) hal_palette(i, (i & 4) ? 15 : 0, (i & 2) ? 15 : 0, (i & 1) ? 15 : 0);
    hal_palette(8, 7, 7, 7);
    for (i = 9; i < 16; i++) hal_palette(i, (i & 4) ? 10 : 0, (i & 2) ? 10 : 0, (i & 1) ? 10 : 0);
}

void g_init(void)
{
    hal_analog(1);
    default_palette();
    g_mode = 3;
    hal_gfx200(0);
    vx1 = 0; vy1 = 0; vx2 = 639; vy2 = 399;
    hal_gpage(0, 0);
    g_apage = 0; g_dpage = 1;   /* = hal_gpage(0, 0): g_dpage is a mask, bit0 = page 0 */
    hal_gclear(0);
    hal_gon(1);
    hal_ton(1);
    g_on = 1;
    /* the rest of the power-on state: a process can host several
     * sessions in turn (engine.c) */
    g_fg = 7; g_bg = 0;
    lastx = 0; lasty = 0;
    win_on = 0;
    wx1 = wy1 = wx2 = wy2 = 0;
}

/* ---- coordinates ---- */
static void to_phys(double x, double y, int *px, int *py)
{
    if (win_on) {
        x = vx1 + (x - wx1) * (vx2 - vx1) / (wx2 - wx1);
        y = vy2 - (y - wy1) * (vy2 - vy1) / (wy2 - wy1);
    }
    if (x < -32768.0) x = -32768.0; if (x > 32767.0) x = 32767.0;
    if (y < -32768.0) y = -32768.0; if (y > 32767.0) y = 32767.0;
    *px = (int)round_away(x); *py = (int)round_away(y);
}

static void parse_coord(int *x, int *y)
{
    int rel = accept(T_STEP);
    double fx, fy;
    expect('(');
    fx = eval_dbl(); expect(','); fy = eval_dbl();
    expect(')');
    if (rel) {
        if (win_on) {
            /* relative in window coords: convert last point back */
            double lx = wx1 + (lastx - vx1) * (wx2 - wx1) / (vx2 - vx1);
            double ly = wy1 + (vy2 - lasty) * (wy2 - wy1) / (vy2 - vy1);
            to_phys(lx + fx, ly + fy, x, y);
        } else { *x = lastx + (int)round_away(fx); *y = lasty + (int)round_away(fy); }
    } else to_phys(fx, fy, x, y);
}

static int is_func_kw(u8 c) { return c == T_PSET || c == T_PRESET || c == T_XOR || c == T_OR || c == T_AND; }

/* ---- statements ---- */
void stmt_screen(void)
{
    int mode = g_mode, sw = -1, ap = g_apage, dp = g_dpage;
    if (peekc() != ',') mode = eval_int();
    if (accept(',')) {
        if (peekc() != ',' && !at_stmt_end()) sw = eval_int();
        if (accept(',')) {
            if (peekc() != ',' && !at_stmt_end()) ap = eval_int();
            if (accept(',')) { if (!at_stmt_end()) dp = eval_int(); }
        }
    }
    if (mode < 0 || mode > 3 || ap < 0 || ap > 1 || dp < 0 || dp > 3) berror(E_ILLEGAL_FN);
    if (mode != g_mode) {
        int was200 = (lines_of() == 200);
        g_mode = mode;
        vx1 = 0; vy1 = 0; vx2 = 639; vy2 = lines_of() - 1;
        win_on = 0;
        /* The mode-change clear must reach the vessel GVRAM when
         * either the old or the new mode is a 200-line mode: leaving
         * 200-line mode with the mirror already off would leave
         * mirrored pixels visible under the new mode, and entering
         * it must wipe the shared VRAM the clear covers. Enable the
         * mirror for the clear, then set it to the new mode's state. */
        hal_gfx200(was200 || lines_of() == 200);
        hal_gpage(0, 0);
        hal_gclear(0);
        hal_gfx200(lines_of() == 200);
    }
    /* display page is a bit mask: bit0 = page 0, bit1 = page 1 (0 = none) */
    g_apage = ap; g_dpage = dp;
    hal_gpage(ap, (dp & 1) ? 0 : (dp & 2) ? 1 : 0);
    if (sw >= 0) {
        /* PC-88 SCREEN 2nd parameter: a 2-bit switch pair (NEC
         * PC-8801mkIIMR N88-BASIC Reference Manual, SCREEN):
         *   bit value 2 = graphics mask: while ON the graphics
         *     patterns are temporarily hidden; their content is
         *     preserved and reappears when the mask turns off.
         *   bit value 1 = fast write: no display effect
         *     (meaningless in V2 mode).
         * SCREEN never affects the TEXT plane -- text is always
         * displayed. So graphics are shown iff !(sw & 2):
         *   sw=0 text + graphics      sw=1 text + graphics
         *   sw=2 text only (masked)   sw=3 text only (masked)
         * Evidence from a genuine M88 recording of MA2026: under
         * SCREEN 0,0 the title shows the PRINTed header line above
         * the XDD graphics (and the playback panel coexists with
         * text-plane indicators); under SCREEN 0,2 (*SELECT) the
         * list stands alone, the graphics preserved underneath.
         * (An earlier mapping, taken from QUASI88 observation --
         * 0 = graphics only, 1 = overlay, 2 = text only -- hid the
         * text plane at sw=0 and showed graphics at sw=3.) */
        g_on = !(sw & 2);
    }
    /* Every SCREEN statement, with or without a switch argument,
     * puts the overlay register (port 0x53) back to "show all": the
     * text a program hid with OUT &H53,1 is back after its next
     * SCREEN (MA2026: hidden at 1440 for play, visible again at
     * *SELECT's SCREEN 0,2 with no OUT in between). */
    hal_pile_reset();
    hal_gon(g_on && dp != 0);
}

void stmt_color(void)
{
    int a = -1, b = -1, c = -1, d = -1;
    if (accept('=')) {
        int p, code;
        expect('('); p = eval_int(); expect(','); code = (int)eval_long(); expect(')');
        if (p < 0 || p > 15) berror(E_ILLEGAL_FN);
        if (code < 16) hal_palette(p, (code & 4) ? 15 : 0, (code & 2) ? 15 : 0, (code & 1) ? 15 : 0);
        else hal_palette(p, (code >> 8) & 15, (code >> 4) & 15, code & 15);
        return;
    }
    if (peekc() != ',') a = eval_int();
    if (accept(',')) {
        if (peekc() != ',' && !at_stmt_end()) b = eval_int();
        if (accept(',')) {
            if (peekc() != ',' && !at_stmt_end()) c = eval_int();
            if (accept(',')) {
                if (peekc() != ',' && !at_stmt_end()) d = eval_int();
                if (accept(',')) eval_int();
            }
        }
    }
    if (a >= 0) {
        if (a > 15) berror(E_ILLEGAL_FN);
        if (con_mono)   /* bit0 secret, bit1 blink, bit2 reverse, bit3 underline */
            text_attr = (u8)(0xE0 | ((a & 1) ? 0 : 1) | (a & 0x0E));
        else {
            text_attr = (u8)((text_attr & 0x1F) | ((a & 7) << 5));
            g_fg = a;
        }
    }
    if (b >= 0) { if (b > 15) berror(E_ILLEGAL_FN); g_bg = b; }
    if (d >= 0) { if (d > 15) berror(E_ILLEGAL_FN); g_fg = d; }
    (void)c;
}

void stmt_pset(int preset)
{
    int x, y, c = preset ? g_bg : g_fg;
    parse_coord(&x, &y);
    if (accept(',')) { if (!at_stmt_end()) c = eval_int(); }
    lastx = x; lasty = y;
    pset_raw(x, y, mapc(c));
}

static void draw_line(int x1, int y1, int x2, int y2, int c, unsigned style)
{
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    unsigned bit = 0x8000;
    if (style == 0xFFFF && y1 == y2) { hline_raw(x1, x2, y1, c); return; }
    for (;;) {
        if (style & bit) pset_raw(x1, y1, c);
        bit >>= 1; if (!bit) bit = 0x8000;
        if (x1 == x2 && y1 == y2) break;
        {
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 < dx) { err += dx; y1 += sy; }
        }
    }
}

/* The ROM's work per LINE statement beyond the flat statement cost:
 * setup plus a per-pixel cost over the drawn length (line), outline
 * (B) or area (BF). Calibrated together with INP and PRINT
 * (docs/APPROXIMATIONS.md item 35). */
#define LINE_BASE_CYCLES  4000L
#define LINE_PIXEL_CYCLES 300L

/* Extent of [a,b] (either order) inside [lo,hi]; 0 when outside. */
static long span_in(int a, int b, int lo, int hi)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (a < lo) a = lo;
    if (b > hi) b = hi;
    return a > b ? 0 : (long)b - a + 1;
}

/* Does the segment (x1,y1)-(x2,y2) touch the view rectangle? Exact
 * (separating axes: the two box axes, then the line's normal). */
static int seg_in_view(int x1, int y1, int x2, int y2)
{
    long long dx = (long long)x2 - x1, dy = (long long)y2 - y1, s;
    int neg = 0, pos = 0, i;
    if ((x1 < vx1 && x2 < vx1) || (x1 > vx2 && x2 > vx2)) return 0;
    if ((y1 < vy1 && y2 < vy1) || (y1 > vy2 && y2 > vy2)) return 0;
    for (i = 0; i < 4; i++) {
        s = dx * ((i & 2 ? vy2 : vy1) - (long long)y1)
          - dy * ((i & 1 ? vx2 : vx1) - (long long)x1);
        if (s < 0) neg = 1; else if (s > 0) pos = 1; else return 1;
    }
    return neg && pos;
}

void stmt_line(void)
{
    int x1 = lastx, y1 = lasty, x2, y2, c = g_fg, box = 0;
    unsigned style = 0xFFFF;
    long pixels;
    if (peekc() != '-') parse_coord(&x1, &y1);
    expect('-');
    {
        int sx = lastx, sy = lasty;
        lastx = x1; lasty = y1;
        parse_coord(&x2, &y2);
        (void)sx; (void)sy;
    }
    if (accept(',')) {
        if (is_func_kw(peekc())) { txtptr++; if (!accept(',')) goto done; }
        if (peekc() != ',' && !at_stmt_end()) c = eval_int();
        if (accept(',')) {
            u8 ch = peekc();
            if (ch == 'B' || ch == 'b') {
                txtptr++; box = 1;
                if (*txtptr == 'F' || *txtptr == 'f') { txtptr++; box = 2; }
            }
            if (accept(',')) {
                if (!at_stmt_end()) style = (unsigned)eval_long();
            }
        }
    }
done:
    c = mapc(c);
    if (box == 0) draw_line(x1, y1, x2, y2, c, style);
    else if (box == 1) {
        draw_line(x1, y1, x2, y1, c, style); draw_line(x2, y1, x2, y2, c, style);
        draw_line(x2, y2, x1, y2, c, style); draw_line(x1, y2, x1, y1, c, style);
    } else {
        int y, ya = y1 < y2 ? y1 : y2, yb = y1 < y2 ? y2 : y1;
        for (y = ya; y <= yb; y++) hline_raw(x1, x2, y, c);
    }
    lastx = x2; lasty = y2;
    /* Pixels the statement covers, clipped to the view like the
     * drawing itself (so far-off coordinates cannot inflate the
     * charge): the longer axis for a line, the four sides as drawn
     * for B (corners twice), the area for BF. A primitive with
     * nothing inside the view charges the base only: the two spans
     * are clipped independently, so a line passing wholly outside
     * (LINE (0,-1)-(639,-1), or a diagonal past a corner) needs the
     * segment test, and a B outline needs both spans non-empty. A
     * line or outline that is partly inside is counted by its clipped
     * spans, not pixel by pixel - an upper bound on the pixels drawn,
     * never more than the full step count. BF's product is already 0
     * when either span is empty. */
    {
        long w = span_in(x1, x2, vx1, vx2), h = span_in(y1, y2, vy1, vy2);
        if (box == 0) pixels = seg_in_view(x1, y1, x2, y2) ? (w > h ? w : h) : 0;
        else if (box == 1) pixels = (w && h) ? 2 * (w + h) : 0;
        else pixels = w * h;
    }
    vessel_charge(LINE_BASE_CYCLES + pixels * LINE_PIXEL_CYCLES);
}

/* ellipse with integer midpoint algorithm */
static void ellipse(int cx, int cy, long rx, long ry, int c, int fill)
{
    long x, y, rx2 = rx * rx, ry2 = ry * ry;
    long px, py, p;
    if (rx <= 0 || ry <= 0) {
        if (rx <= 0 && ry <= 0) { pset_raw(cx, cy, c); return; }
        if (rx <= 0) { draw_line(cx, cy - (int)ry, cx, cy + (int)ry, c, 0xFFFF); return; }
        hline_raw(cx - (int)rx, cx + (int)rx, cy, c); return;
    }
    if (fill) {
        for (y = 0; y <= ry; y++) {
            double t = 1.0 - (double)y * y / ((double)ry * ry);
            long xx = (long)(rx * sqrt(t < 0 ? 0 : t) + 0.5);
            hline_raw(cx - (int)xx, cx + (int)xx, cy + (int)y, c);
            if (y) hline_raw(cx - (int)xx, cx + (int)xx, cy - (int)y, c);
        }
        return;
    }
    x = 0; y = ry;
    px = 0; py = 2 * rx2 * y;
    p = ry2 - rx2 * ry + rx2 / 4;
#define PLOT4(X, Y) do { \
        pset_raw(cx + (int)(X), cy + (int)(Y), c); pset_raw(cx - (int)(X), cy + (int)(Y), c); \
        pset_raw(cx + (int)(X), cy - (int)(Y), c); pset_raw(cx - (int)(X), cy - (int)(Y), c); } while (0)
    while (px < py) {
        PLOT4(x, y);
        x++; px += 2 * ry2;
        if (p < 0) p += ry2 + px;
        else { y--; py -= 2 * rx2; p += ry2 + px - py; }
    }
    p = (long)((double)ry2 * (x + 0.5) * (x + 0.5) + (double)rx2 * (y - 1) * (y - 1) - (double)rx2 * ry2);
    while (y >= 0) {
        PLOT4(x, y);
        y--; py -= 2 * rx2;
        if (p > 0) p += rx2 - py;
        else { x++; px += 2 * ry2; p += rx2 - py + px; }
    }
#undef PLOT4
}

void stmt_circle(void)
{
    int cx, cy, c = g_fg, fill = 0;
    double r, start = 0, end = 0, ratio = -1;
    int has_start = 0, has_end = 0;
    parse_coord(&cx, &cy);
    expect(',');
    r = eval_dbl();
    if (accept(',')) {
        if (peekc() != ',' && !at_stmt_end()) c = eval_int();
        if (accept(',')) {
            if (peekc() != ',' && !at_stmt_end()) { start = eval_dbl(); has_start = 1; }
            if (accept(',')) {
                if (peekc() != ',' && !at_stmt_end()) { end = eval_dbl(); has_end = 1; }
                if (accept(',')) {
                    if (peekc() != ',' && !at_stmt_end()) ratio = eval_dbl();
                    if (accept(',')) {
                        u8 ch = peekc();
                        if (ch == 'F' || ch == 'f') { txtptr++; fill = 1; }
                        if (accept(',')) { if (!at_stmt_end()) eval_dbl(); }
                    }
                }
            }
        }
    }
    if (win_on) r = r * (vx2 - vx1) / fabs(wx2 - wx1);
    if (ratio < 0) ratio = (lines_of() == 200) ? 0.5 : 1.0;
    c = mapc(c);
    lastx = cx; lasty = cy;
    {
        long rx, ry;
        if (ratio <= 1) { rx = (long)round_away(r); ry = (long)round_away(r * ratio); }
        else { rx = (long)round_away(r / ratio); ry = (long)round_away(r); }
        if (!has_start && !has_end) { ellipse(cx, cy, rx, ry, c, fill); return; }
        {
            /* arc by angle stepping */
            double a0 = fabs(start), a1 = has_end ? fabs(end) : 6.283185307179586;
            double step, a;
            int px0, py0, first = 1, steps;
            if (a1 < a0) a1 += 6.283185307179586;
            steps = (int)((a1 - a0) * (rx > ry ? rx : ry) / 2) + 8;
            if (steps > 720) steps = 720;
            step = (a1 - a0) / steps;
            for (a = a0; ; a += step) {
                int px, py;
                if (a > a1) a = a1;
                px = cx + (int)round_away(rx * cos(a));
                py = cy - (int)round_away(ry * sin(a));
                if (!first) draw_line(px0, py0, px, py, c, 0xFFFF);
                first = 0; px0 = px; py0 = py;
                if (a >= a1) break;
            }
            if (has_start && start < 0) draw_line(cx, cy, cx + (int)round_away(rx * cos(a0)), cy - (int)round_away(ry * sin(a0)), c, 0xFFFF);
            if (has_end && end < 0) draw_line(cx, cy, cx + (int)round_away(rx * cos(a1)), cy - (int)round_away(ry * sin(a1)), c, 0xFFFF);
        }
    }
}

void stmt_paint(void)
{
    int x, y, c = g_fg, bc = -1;
    int *stack; int sp = 0, maxsp = 4000;
    parse_coord(&x, &y);
    if (accept(',')) {
        if (peekc() != ',' && !at_stmt_end()) {
            if (peekc() == '"') { VAL t; eval(&t); }  /* tiling not supported: use fg */
            else c = eval_int();
        }
        if (accept(',')) { if (peekc() != ',' && !at_stmt_end()) bc = eval_int(); if (accept(',')) { if (!at_stmt_end()) eval_int(); } }
    }
    c = mapc(c);
    bc = (bc < 0) ? c : mapc(bc);
    lastx = x; lasty = y;
    if (x < vx1 || x > vx2 || y < vy1 || y > vy2) return;
    {
        int pc = point_raw(x, y);
        if (pc == bc || pc == c) return;
    }
    stack = (int *)malloc(sizeof(int) * 2 * maxsp);
    if (!stack) berror(E_OUT_OF_MEM);
    stack[sp++] = x; stack[sp++] = y;
    while (sp > 0) {
        int sy = stack[--sp], sx = stack[--sp];
        int l, r, xx;
        int p = point_raw(sx, sy);
        if (p == bc || p == c) continue;
        l = sx; while (l > vx1) { int q = point_raw(l - 1, sy); if (q == bc || q == c) break; l--; }
        r = sx; while (r < vx2) { int q = point_raw(r + 1, sy); if (q == bc || q == c) break; r++; }
        hline_raw(l, r, sy, c);
        {
            int dy;
            for (dy = -1; dy <= 1; dy += 2) {
                int ny = sy + dy, inrun = 0;
                if (ny < vy1 || ny > vy2) continue;
                for (xx = l; xx <= r; xx++) {
                    int q = point_raw(xx, ny);
                    if (q != bc && q != c) {
                        if (!inrun) {
                            if (sp + 2 > maxsp * 2) {
                                int *ns;
                                if (maxsp >= 7000) { free(stack); berror(E_OUT_OF_MEM); }
                                ns = (int *)realloc(stack, sizeof(int) * 4 * maxsp);
                                if (!ns) { free(stack); berror(E_OUT_OF_MEM); }
                                stack = ns; maxsp *= 2;
                            }
                            stack[sp++] = xx; stack[sp++] = ny; inrun = 1;
                        }
                    } else inrun = 0;
                }
            }
        }
        if (hal_breakflag()) { free(stack); berror(E_ILLEGAL_FN); }
    }
    free(stack);
}

int fn_point(void)
{
    int x, y;
    expect('(');
    if (peekc() == T_STEP) { txtptr++; }
    {
        double fx = eval_dbl(), fy;
        if (!accept(',')) {
            /* POINT(n) */
            int n = (int)fx;
            expect(')');
            switch (n) { case 0: case 2: return lastx; case 1: case 3: return lasty; }
            berror(E_ILLEGAL_FN);
        }
        fy = eval_dbl();
        expect(')');
        to_phys(fx, fy, &x, &y);
    }
    if (x < vx1 || x > vx2 || y < vy1 || y > vy2) return -1;
    return point_raw(x, y);
}

void stmt_view(void)
{
    int x1, y1, x2, y2, fc = -1, bc = -1;
    if (at_stmt_end()) { vx1 = 0; vy1 = 0; vx2 = 639; vy2 = lines_of() - 1; return; }
    accept(T_SCREEN);
    expect('('); x1 = eval_int(); expect(','); y1 = eval_int(); expect(')');
    expect('-');
    expect('('); x2 = eval_int(); expect(','); y2 = eval_int(); expect(')');
    if (accept(',')) { if (peekc() != ',' && !at_stmt_end()) fc = eval_int(); if (accept(',')) bc = eval_int(); }
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x1 < 0 || x2 > 639 || y1 < 0 || y2 >= lines_of()) berror(E_ILLEGAL_FN);
    if (bc >= 0) {
        int sx1 = x1 - 1, sy1 = y1 - 1, sx2 = x2 + 1, sy2 = y2 + 1;
        vx1 = 0; vy1 = 0; vx2 = 639; vy2 = lines_of() - 1;
        draw_line(sx1, sy1, sx2, sy1, mapc(bc), 0xFFFF); draw_line(sx2, sy1, sx2, sy2, mapc(bc), 0xFFFF);
        draw_line(sx2, sy2, sx1, sy2, mapc(bc), 0xFFFF); draw_line(sx1, sy2, sx1, sy1, mapc(bc), 0xFFFF);
    }
    vx1 = x1; vy1 = y1; vx2 = x2; vy2 = y2;
    if (fc >= 0) { int y; for (y = vy1; y <= vy2; y++) hline_raw(vx1, vx2, y, mapc(fc)); }
}

void stmt_window(void)
{
    if (at_stmt_end()) { win_on = 0; return; }
    accept(T_SCREEN);
    expect('('); wx1 = eval_dbl(); expect(','); wy1 = eval_dbl(); expect(')');
    expect('-');
    expect('('); wx2 = eval_dbl(); expect(','); wy2 = eval_dbl(); expect(')');
    if (wx1 == wx2 || wy1 == wy2) berror(E_ILLEGAL_FN);
    if (wx1 > wx2) { double t = wx1; wx1 = wx2; wx2 = t; }
    if (wy1 > wy2) { double t = wy1; wy1 = wy2; wy2 = t; }
    win_on = 1;
}

/* GET@ (x1,y1)-(x2,y2), array   /  PUT@ (x,y), array [,mode] */
void stmt_getput_gfx(int is_put)
{
    int x1, y1, x2 = 0, y2 = 0, mode = T_PSET, w, h, rowb, x, y, pl;
    char name[41]; int t;
    long bytes;
    u8 *base;
    parse_coord(&x1, &y1);
    if (!is_put) { expect('-'); parse_coord(&x2, &y2); }
    expect(',');
    peekc();
    parse_varname(name, &t);
    if (t == T_STR) berror(E_TYPE);
    if (accept('(')) { do { eval_int(); } while (accept(',')); expect(')'); }
    base = (u8 *)array_base(name, t, &bytes);
    if (!base) berror(E_ILLEGAL_FN);
    if (is_put && accept(',')) {
        u8 c = peekc();
        if (is_func_kw(c)) { mode = c; txtptr++; }
        if (accept(',')) eval_int();
    }
    if (!is_put) {
        if (x1 > x2) { int tt = x1; x1 = x2; x2 = tt; }
        if (y1 > y2) { int tt = y1; y1 = y2; y2 = tt; }
        w = x2 - x1 + 1; h = y2 - y1 + 1;
        rowb = (w + 7) / 8;
        if (4L + (long)rowb * h * 4 > bytes) berror(E_ILLEGAL_FN);
        base[0] = (u8)(w & 0xFF); base[1] = (u8)(w >> 8);
        base[2] = (u8)(h & 0xFF); base[3] = (u8)(h >> 8);
        memset(base + 4, 0, (size_t)rowb * h * 4);
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            int c = point_raw(x1 + x, y1 + y);
            if (c < 0) c = 0;
            for (pl = 0; pl < 4; pl++)
                if (c & (1 << pl)) base[4 + ((long)pl * h + y) * rowb + x / 8] |= (u8)(0x80 >> (x & 7));
        }
        return;
    }
    w = base[0] | (base[1] << 8); h = base[2] | (base[3] << 8);
    rowb = (w + 7) / 8;
    if (w <= 0 || h <= 0 || 4L + (long)rowb * h * 4 > bytes) berror(E_ILLEGAL_FN);
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        int c = 0, d, r;
        for (pl = 0; pl < 4; pl++)
            if (base[4 + ((long)pl * h + y) * rowb + x / 8] & (0x80 >> (x & 7))) c |= 1 << pl;
        if (x1 + x > 639 || y1 + y >= lines_of()) continue;
        d = point_raw(x1 + x, y1 + y); if (d < 0) d = 0;
        switch (mode) {
        case T_PRESET: r = (~c) & 15; break;
        case T_XOR: r = d ^ c; break;
        case T_OR: r = d | c; break;
        case T_AND: r = d & c; break;
        default: r = c; break;
        }
        pset_raw(x1 + x, y1 + y, r);
    }
}

/* STICK / STRIG via keyboard matrix */
int fn_stick(int n)
{
    int up, dn, lt, rt;
    if (n != 0) return 0;
    /* scan codes: 3A up, 3B left, 3C right, 3D down ; tenkey 43(8) 46(4) 48(6) 4B(2) */
    up = (hal_keymatrix(0x3A >> 3) & (1 << (0x3A & 7))) || (hal_keymatrix(0x43 >> 3) & (1 << (0x43 & 7)));
    lt = (hal_keymatrix(0x3B >> 3) & (1 << (0x3B & 7))) || (hal_keymatrix(0x46 >> 3) & (1 << (0x46 & 7)));
    rt = (hal_keymatrix(0x3C >> 3) & (1 << (0x3C & 7))) || (hal_keymatrix(0x48 >> 3) & (1 << (0x48 & 7)));
    dn = (hal_keymatrix(0x3D >> 3) & (1 << (0x3D & 7))) || (hal_keymatrix(0x4B >> 3) & (1 << (0x4B & 7)));
    if (up && rt) return 2;
    if (dn && rt) return 4;
    if (dn && lt) return 6;
    if (up && lt) return 8;
    if (up) return 1;
    if (rt) return 3;
    if (dn) return 5;
    if (lt) return 7;
    return 0;
}

int fn_strig(int n)
{
    if (n != 0) return 0;
    return (hal_keymatrix(0x34 >> 3) & (1 << (0x34 & 7))) ? -1 : 0;
}
