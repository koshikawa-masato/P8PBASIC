/* halhost.c - host (Linux) HAL for testing the interpreter core */
#include "basic.h"
#include "vessel.h"
#include <time.h>

static unsigned short tv[25][80];
static unsigned char ta[25][80];
static unsigned char gv[2][400][640];
static int ap = 0;
static int w40 = 0;
static const char *keys;
static int keypos = 0;
static unsigned long tick = 0;
static int brk = 0;
static int idle_count = 0;
static unsigned char kmatrix[16];
static int inp_direct = 0;
static unsigned char memory[65536];

static void dump(void)
{
    int y, x;
    const char *gf = getenv("P9PGFX");
    printf("----- SCREEN -----\n");
    for (y = 0; y < 25; y++) {
        char line[200]; int o = 0;
        for (x = 0; x < 80; x++) {
            unsigned short v = tv[y][x];
            if ((v & 0xFF00) && !(v & 0x80)) {
                unsigned c1 = (v & 0x7F) + 0x20, c2 = (v >> 8) & 0x7F, s1, s2;
                s1 = ((c1 + 1) >> 1) + (c1 <= 0x5E ? 0x70 : 0xB0);
                s2 = c2 + ((c1 & 1) ? (c2 >= 0x60 ? 0x20 : 0x1F) : 0x7E);
                line[o++] = (char)s1; line[o++] = (char)s2;
            }
            else if (v & 0xFF00) { }
            else line[o++] = (v >= 0x20 && v < 0x7F) ? (char)v : (v == 0 ? ' ' : '?');
        }
        while (o > 0 && line[o - 1] == ' ') o--;
        line[o] = 0;
        printf("%02d|%s\n", y, line);
    }
    if (gf) {
        FILE *f = fopen(gf, "wb");
        int pal[16][3] = {{0,0,0},{0,0,255},{255,0,0},{255,0,255},{0,255,0},{0,255,255},{255,255,0},{255,255,255},
            {128,128,128},{0,0,160},{160,0,0},{160,0,160},{0,160,0},{0,160,160},{160,160,0},{160,160,160}};
        fprintf(f, "P6 640 400 255\n");
        for (y = 0; y < 400; y++) for (x = 0; x < 640; x++) {
            int c = gv[0][y][x];
            fputc(pal[c][0], f); fputc(pal[c][1], f); fputc(pal[c][2], f);
        }
        fclose(f);
    }
    if (getenv("P8P_VRAMDUMP")) {
        /* mirror tools/embed_test.c: dump the vessel's VRAM planes
         * so host probes can verify Z80-side drawing (XDD, XDE 3) */
        int b;
        for (b = 0; b < 3; b++) {
            char path[32];
            FILE *f;
            snprintf(path, sizeof(path), "vram%d.bin", b);
            f = fopen(path, "wb");
            if (f) { fwrite(vessel_vram_plane(b), 1, 0x4000, f); fclose(f); }
        }
    }
}

void hal_init(int f)
{
    (void)f;
    memset(kmatrix, 0xFF, sizeof(kmatrix));   /* idle = nothing pressed */
    keys = getenv("P9PKEYS");
    if (!keys) keys = "";
    atexit(dump);
}
void hal_term(void) { }

void hal_tput(int x, int y, unsigned short v, unsigned char a)
{
    if (w40) x *= 2;
    if (x >= 0 && x < 80 && y >= 0 && y < 25) { tv[y][x] = v; ta[y][x] = a; }
}

void hal_trow(int y, const unsigned short *codes, const unsigned char *attrs, int n)
{
    int x;
    for (x = 0; x < n; x++) hal_tput(x, y, codes[x], attrs[x]);
}

void hal_cursor(int x, int y, int v, int ins) { (void)x; (void)y; (void)v; (void)ins; }
void hal_width40(int on) { w40 = on; }
void hal_gfx200(int on) { (void)on; }

/* key script: chars; escapes \r \n \e(ESC) \s(STOP) \u \d \l \R(right) \1..\9 \0 (F1..F10) \w (wait 1 tick) */
static int pending = -1;
int hal_getkey(void)
{
    char c;
    if (pending >= 0) { int k = pending; pending = -1; return k; }
    c = keys[keypos];
    if (!c) return -1;
    keypos++;
    if (c != '\\') return (unsigned char)c;
    c = keys[keypos++];
    switch (c) {
    case 'r': return 13;
    case 'n': return 13;
    case 'e': return 27;
    case 'b': return 8;
    case 's': brk = 1; return -1;
    case 'u': return 0x1E;
    case 'd': return 0x1F;
    case 'l': return 0x1D;
    case 'R': return 0x1C;
    case 'i': return 0x12;
    case 'x': return 0x7F;
    case 'h': return 0x0B;
    case 'w': return -1;
    case '\\': return '\\';
    default:
        if (c >= '1' && c <= '9') return KC_F1 + (c - '1');
        if (c == '0') return KC_F1 + 9;
        return c;
    }
}

int hal_keyhit(void) { return keys[keypos] != 0; }
int hal_breakflag(void) { int b = brk; brk = 0; return b; }
unsigned char hal_lastscan(void) { return 0; }
unsigned char hal_keymatrix(int i) { return kmatrix[i & 15]; }
int hal_shiftstate(void) { return 0; }
void hal_set_inp_direct(int on) { inp_direct = on; }
int hal_inp_direct(void) { return inp_direct; }

unsigned long hal_ticks(void) { return tick++; }
void hal_beep(int on) { (void)on; }
void hal_idle(void)
{
    tick++;
    if (!keys[keypos]) {
        if (++idle_count > 2000) { exit(0); }
    } else idle_count = 0;
}

/* BASIC's INP/OUT go through the vessel's PC-88 port model, so the
 * interpreter and the Z80 see the same machine (sound board probe,
 * disk subsystem, ...). Ports the model does not know read as 0xFF,
 * which is also the idle keyboard matrix. */
int hal_inp(unsigned port) { return vessel_port_in((int)(port & 0xFF)); }
void hal_outp(unsigned port, int val) { vessel_port_out((int)(port & 0xFF), val); }
int hal_peek(unsigned seg, unsigned off) { (void)seg; return memory[off & 0xFFFF]; }
void hal_poke(unsigned seg, unsigned off, int v) { (void)seg; memory[off & 0xFFFF] = (unsigned char)v; }
unsigned hal_dataseg(void) { return 0x1000; }
unsigned long hal_varptr(void *p) { return (unsigned long)((size_t)p & 0xFFFF) | 0x10000000UL; }
void hal_call(unsigned seg, unsigned off) { (void)seg; (void)off; }

void hal_gon(int on) { (void)on; }
void hal_ton(int on) { (void)on; }
void hal_pile_reset(void) { }
void hal_text_clear_region(int top, int bot) { (void)top; (void)bot; }
void hal_gpage(int a, int d) { ap = a; (void)d; }
void hal_gpset(int x, int y, int c) { if (x >= 0 && x < 640 && y >= 0 && y < 400) gv[ap][y][x] = (unsigned char)c; }
int hal_gpoint(int x, int y) { if (x >= 0 && x < 640 && y >= 0 && y < 400) return gv[ap][y][x]; return -1; }
void hal_ghline(int x1, int x2, int y, int c) { int x; for (x = x1; x <= x2; x++) hal_gpset(x, y, c); }
void hal_gclear(int c) { memset(gv[ap], c, sizeof(gv[0])); }
void hal_palette(int i, int g, int r, int b) { (void)i; (void)g; (void)r; (void)b; }
void hal_analog(int on) { (void)on; }

void hal_gettime(int *h, int *m, int *s) { *h = 12; *m = 34; *s = 56; }
void hal_getdate(int *y, int *mo, int *d) { *y = 2026; *mo = 11; *d = 16; }
void hal_settime(int h, int m, int s) { (void)h; (void)m; (void)s; }
void hal_setdate(int y, int mo, int d) { (void)y; (void)mo; (void)d; }

void hal_poll(void) { }
