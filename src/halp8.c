/* halp8.c - embedded HAL for the P8PBASIC engine (engine.c).
 *   Same in-memory machine model as the host HAL (halhost.c):
 *   text VRAM 80x25, graphics 640x400 in palette indices (2 pages),
 *   one flat 64 KiB memory shared with the Z80 vessel. Differences:
 *   keys arrive from the host application through a thread-safe
 *   queue, the clock is virtual (advanced by idling), and hal_term
 *   is the engine's exit hatch (see p8internal.h). */
#include "basic.h"
#include "vessel.h"
#include "halp8.h"
#include "p8internal.h"
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static unsigned short tv[25][80];
static unsigned char ta[25][80];
static unsigned char gv[2][400][640];
static int ap = 0, dp = 0;
static int w40 = 0;
static int hcur_x = 0, hcur_y = 0, hcur_vis = 0;
static unsigned long tick = 0;
static int brkflag = 0;
static unsigned char kmatrix[16];
static int inp_direct = 0;
static unsigned char memory[65536];

/* ---- key queue (producer: host app, consumer: interpreter) ---- */
#define KQ_SIZE 256
static int kq[KQ_SIZE];
static int kq_head = 0, kq_tail = 0;
static pthread_mutex_t kq_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int stop_req = 0;

void halp8_push_key(int code)
{
    pthread_mutex_lock(&kq_mu);
    if ((kq_tail + 1) % KQ_SIZE != kq_head) {
        kq[kq_tail] = code;
        kq_tail = (kq_tail + 1) % KQ_SIZE;
    }
    pthread_mutex_unlock(&kq_mu);
}

static int kq_pop(void)
{
    int v = -1;
    pthread_mutex_lock(&kq_mu);
    if (kq_head != kq_tail) {
        v = kq[kq_head];
        kq_head = (kq_head + 1) % KQ_SIZE;
    }
    pthread_mutex_unlock(&kq_mu);
    return v;
}

static int kq_count(void)
{
    int n;
    pthread_mutex_lock(&kq_mu);
    n = (kq_tail - kq_head + KQ_SIZE) % KQ_SIZE;
    pthread_mutex_unlock(&kq_mu);
    return n;
}

void halp8_set_matrix(int idx, unsigned char v) { kmatrix[idx & 15] = v; }
void halp8_request_stop(void) { stop_req = 1; }
int  halp8_stop_requested(void) { return stop_req; }

static volatile int paused_flag = 0;
void halp8_set_paused(int paused) { paused_flag = paused; }
int  halp8_paused(void) { return paused_flag; }

static int gon_flag = 1, ton_flag = 1;
static int gfx200 = 0;
static int pal[16][3];
static int pal_init = 0;
static int poll_n = 0;              /* hal_poll's statement counter */

/* Put every per-session static back to its power-on value, so a new
 * engine in the same process starts from the state a fresh process
 * would have. Called by p8_start() before the engine thread exists
 * (no interpreter thread is running), and BEFORE
 * vessel_session_reset(): the vessel's setup pokes its stubs into
 * the guest RAM cleared here. */
void halp8_session_reset(void)
{
    stop_req = 0;
    paused_flag = 0;
    pthread_mutex_lock(&kq_mu);
    kq_head = kq_tail = 0;
    pthread_mutex_unlock(&kq_mu);
    memset(memory, 0, sizeof(memory));
    memset(tv, 0, sizeof(tv));
    memset(ta, 0, sizeof(ta));
    memset(gv, 0, sizeof(gv));
    ap = dp = 0;
    w40 = 0;
    gfx200 = 0;
    hcur_x = hcur_y = hcur_vis = 0;
    memset(pal, 0, sizeof(pal));
    pal_init = 0;                   /* re-initialises on first use */
    gon_flag = 1; ton_flag = 1;
    tick = 0;
    brkflag = 0;
    inp_direct = 0;
    memset(kmatrix, 0xFF, sizeof(kmatrix));   /* idle = nothing pressed */
    poll_n = 0;
}

const unsigned short *halp8_text_codes(void) { return &tv[0][0]; }
const unsigned char  *halp8_text_attrs(void) { return &ta[0][0]; }
const unsigned char  *halp8_gfx(int page) { return &gv[page & 1][0][0]; }
int  halp8_gfx_display_page(void) { return dp; }
int  halp8_width40(void) { return w40; }
void halp8_cursor(int *x, int *y, int *visible)
{
    *x = hcur_x; *y = hcur_y; *visible = hcur_vis;
}

void hal_init(int f)
{
    (void)f;
    memset(kmatrix, 0xFF, sizeof(kmatrix));   /* idle = nothing pressed */
}
void hal_term(void) { p8_bail(); }

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

void hal_cursor(int x, int y, int v, int ins)
{
    (void)ins;
    hcur_x = x; hcur_y = y; hcur_vis = v;
}
void hal_width40(int on) { w40 = on; }

int hal_getkey(void)
{
    if (stop_req) p8_bail();
    return kq_pop();
}

int hal_keyhit(void) { return kq_count() > 0; }
int hal_breakflag(void) { int b = brkflag; brkflag = 0; return b; }
unsigned char hal_lastscan(void) { return 0; }
unsigned char hal_keymatrix(int i) { return kmatrix[i & 15]; }
int hal_shiftstate(void) { return 0; }
void hal_set_inp_direct(int on) { inp_direct = on; }
int hal_inp_direct(void) { return inp_direct; }

unsigned long hal_ticks(void) { return tick++; }
void hal_beep(int on) { (void)on; }
void hal_idle(void)
{
    if (stop_req) p8_bail();
    while (paused_flag && !stop_req) usleep(10000);
    if (stop_req) p8_bail();
    tick++;
    /* When the guest is waiting for input there is nothing to
     * compute; yield real time so the interpreter does not spin
     * a core flat out inside the host application. */
    if (kq_count() == 0) usleep(1000);
}

/* BASIC's INP/OUT go through the vessel's PC-88 port model, exactly
 * as in the host HAL. */
int hal_inp(unsigned port) { return vessel_port_in((int)(port & 0xFF)); }
void hal_outp(unsigned port, int val) { vessel_port_out((int)(port & 0xFF), val); }
int hal_peek(unsigned seg, unsigned off) { (void)seg; return memory[off & 0xFFFF]; }
void hal_poke(unsigned seg, unsigned off, int v) { (void)seg; memory[off & 0xFFFF] = (unsigned char)v; }
unsigned hal_dataseg(void) { return 0x1000; }
unsigned long hal_varptr(void *p) { return (unsigned long)((size_t)p & 0xFFFF) | 0x10000000UL; }
void hal_call(unsigned seg, unsigned off) { (void)seg; (void)off; }

void hal_gon(int on) { gon_flag = on; }
void hal_ton(int on) { ton_flag = on; }
void hal_pile_reset(void) { vessel_pile_reset(); }
/* CLS reaches the guest text image too: on hardware the interpreter's
 * screen and the image at 0xF3C8 are one memory (25 rows x 120 bytes:
 * 80 codes + 20 attribute pairs), so text that only machine code
 * wrote there must not survive a CLS. */
void hal_text_clear_region(int top, int bot)
{
    if (top < 0) top = 0;
    if (bot > 24) bot = 24;
    if (top > bot) return;
    memset(memory + 0xF3C8 + top * 120, 0, (size_t)(bot - top + 1) * 120);
}
int halp8_gfx_visible(void) { return gon_flag; }
int halp8_text_visible(void) { return ton_flag; }
/* Writes to the displayed page are mirrored into the vessel's GVRAM
 * (one shared VRAM on the genuine machine); a hidden page is not.
 * The vessel has 200 lines and 3 planes, so mirroring happens only
 * in the interpreter's 200-line modes (hal_gfx200), and colors
 * above 7 clear the vessel bits so the interpreter's own color
 * shows through the frame compositor. */
void hal_gfx200(int on) { gfx200 = on; }
static int vcol(int c) { return (c >= 0 && c <= 7) ? c : 0; }
void hal_gpage(int a, int d)
{
    int changed = (d != dp);
    ap = a; dp = d;
    if (changed && gfx200 && dp >= 0 && dp < 2) {
        /* Display page changed: resync the single GVRAM from it.
         * One vessel line serves the frame row pair (2y, 2y+1); in
         * 200-line mode the interpreter writes both rows of a pair
         * identically, so sample the even row - reading both rows
         * would let the odd row erase the even one's pixels. */
        int x, y;
        for (y = 0; y < 200; y++)
            for (x = 0; x < 640; x++)
                vessel_vram_pset(x, y * 2, vcol(gv[dp][y * 2][x]));
    }
}
void hal_gpset(int x, int y, int c) { if (x >= 0 && x < 640 && y >= 0 && y < 400) { gv[ap][y][x] = (unsigned char)c; if (ap == dp && gfx200 && c >= 0) vessel_vram_pset(x, y, vcol(c)); } }
/* On the displayed page a non-zero vessel pixel wins, as in the frame. */
int hal_gpoint(int x, int y)
{
    if (x >= 0 && x < 640 && y >= 0 && y < 400) {
        if (ap == dp && gfx200) { int v = vessel_vram_gpoint(x, y); if (v != 0) return v; }
        return gv[ap][y][x];
    }
    return -1;
}
void hal_ghline(int x1, int x2, int y, int c) { int x; for (x = x1; x <= x2; x++) hal_gpset(x, y, c); }
void hal_gclear(int c) { memset(gv[ap], c, sizeof(gv[0])); if (ap == dp && gfx200 && c >= 0) vessel_vram_clear(vcol(c)); }

void hal_palette(int i, int g, int r, int b)
{
    if (!pal_init) {
        static const int def[16][3] = {{0,0,0},{0,0,15},{15,0,0},{15,0,15},{0,15,0},{0,15,15},{15,15,0},{15,15,15},
            {8,8,8},{0,0,10},{10,0,0},{10,0,10},{0,10,0},{0,10,10},{10,10,0},{10,10,10}};
        memcpy(pal, def, sizeof(pal));
        pal_init = 1;
    }
    if (i >= 0 && i < 16) { pal[i][0] = r; pal[i][1] = g; pal[i][2] = b; }
    /* The interpreter's PALETTE statement programs the same hardware
     * palette registers that machine code writes via ports 0x54-0x5B;
     * mirror entries 0..7 (4-bit values scaled to the registers'
     * 3 bits) so both writers share one set, as on hardware. */
    if (i >= 0 && i < 8)
        vessel_set_palette(i, (r * 7 + 7) / 15, (g * 7 + 7) / 15, (b * 7 + 7) / 15);
}
void hal_analog(int on) { (void)on; }

/* palette lookup for the engine's frame compositor (engine.c) */
void halp8_palette(int idx, int *r, int *g, int *b)
{
    if (!pal_init) hal_palette(-1, 0, 0, 0);
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    *r = pal[idx][0] * 17; *g = pal[idx][1] * 17; *b = pal[idx][2] * 17;
}

void hal_gettime(int *h, int *m, int *s)
{
    time_t t = time(0);
    struct tm tmv;
    localtime_r(&t, &tmv);
    *h = tmv.tm_hour; *m = tmv.tm_min; *s = tmv.tm_sec;
}
void hal_getdate(int *y, int *mo, int *d)
{
    time_t t = time(0);
    struct tm tmv;
    localtime_r(&t, &tmv);
    *y = tmv.tm_year + 1900; *mo = tmv.tm_mon + 1; *d = tmv.tm_mday;
}
void hal_settime(int h, int m, int s) { (void)h; (void)m; (void)s; }
void hal_setdate(int y, int mo, int d) { (void)y; (void)mo; (void)d; }

/* Per-statement poll from exec_loop: the engine's stop request
 * lands here even when the guest never waits for input. This is
 * also the pacing point: once music is rendering, the interpreter
 * must not run ahead of the audio clock, or the host's ring buffer
 * overflows and playback tears. Checked every 64 statements: one check interval pumps ~0.27 s of emulated time, which the 2 s ring absorbs. */
void hal_poll(void)
{
    if (stop_req) p8_bail();
    while (paused_flag && !stop_req) usleep(10000);
    if (stop_req) p8_bail();
    if (++poll_n < 64) return;
    poll_n = 0;
    /* Each statement pumps ~4 ms of emulated time, so a fast host
     * produces seconds of audio per check interval; sleep off the
     * whole lead (in stop-responsive chunks), not a fixed cap. */
    while (!stop_req) {
        double ahead = p8_audio_seconds() - p8_wall_seconds();
        long us;
        if (ahead <= 0.15) break;
        us = (long)((ahead - 0.10) * 1000000.0);
        if (us > 50000) us = 50000;
        if (us <= 0) break;
        usleep((useconds_t)us);
    }
    if (stop_req) p8_bail();
}
