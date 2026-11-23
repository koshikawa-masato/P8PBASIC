/* halp8.c - embedded HAL for the P8PBASIC engine (engine.c).
 *   Same in-memory machine model as the host HAL (halhost.c):
 *   text VRAM 80x25, graphics 640x400 in palette indices (2 pages),
 *   one flat 64 KiB memory shared with the Z80 vessel. Differences:
 *   keys arrive from the host application as the key matrix (the
 *   HAL generates the characters from it, see keyscan) or through a
 *   thread-safe queue, the clock is virtual (advanced by idling), and hal_term
 *   is the engine's exit hatch (see p8internal.h). */
#include "basic.h"
#include "hal.h"
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
static pthread_mutex_t km_mu = PTHREAD_MUTEX_INITIALIZER;
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

/* Push only into an empty queue, atomically (the scanner's repeat
 * uses this so a repeat can never land behind an unread key). */
static int kq_push_if_empty(int code)
{
    int done = 0;
    pthread_mutex_lock(&kq_mu);
    if (kq_head == kq_tail) {
        kq[kq_tail] = code;
        kq_tail = (kq_tail + 1) % KQ_SIZE;
        done = 1;
    }
    pthread_mutex_unlock(&kq_mu);
    return done;
}

/* ---- key buffer (release filter between the host feed and the guest) ----
 * The host writes the RAW matrix; the guest (keyscan, hal_keymatrix =
 * INP ports 00-0E) reads kmatrix, the VISIBLE one. kb_tick() promotes
 * raw -> visible per cell, on the interpreter thread, and the rule is
 * asymmetric:
 *  - a press (raw down, visible up) becomes visible at once, at the
 *    first tick that sees it: a press edge from the host is
 *    deliberate, and any wait here is felt as sluggish keys;
 *  - a release (raw up, visible down) becomes visible only once the
 *    raw release has been stable for KB_RELEASE_MS and the visible
 *    press has lasted KB_MIN_PRESS_MS, so once a tap is promoted it
 *    cannot fall between two guest polls. (A press that starts and
 *    ends entirely between two ticks is never promoted at all.)
 * The releases are where a host feed flaps: a touch-assignment flip
 * or a drifting controller stick produces release + re-press PAIRS
 * within a few ms, which the genuine keyboard does not. A pair
 * shorter than KB_RELEASE_MS is absorbed: the visible cell simply
 * stays down through it. A release must therefore be stable for
 * 25 ms before the guest sees it (10 ms at first; raised after a
 * device report of residual chatter in cursor movement, where the
 * pairs ran longer than 10 ms). A deliberate release still lands
 * within one poll of those 25 ms; the press side is untouched.
 * Snapshot consistency (pull-through): keyscan translates a press
 * edge with the modifier levels (SHIFT, GRPH, CAPS, kana) of the same
 * visible snapshot, and a genuine matrix is scanned as one consistent
 * snapshot. So a press must not overtake a pending release: in a tick
 * that promotes at least one press, every pending release whose
 * visible press has already lasted KB_MIN_PRESS_MS is promoted too,
 * without the KB_RELEASE_MS stability wait. (Release SHIFT, press a
 * letter at once: the letter is seen unshifted.) Releases short of
 * the minimum press stay pending. The cost: a flap's release half is
 * let through if ANOTHER cell's press arrives during it; with no
 * other press the pair is absorbed as before (a same-cell re-press
 * is not a press promotion: the visible cell never went up).
 * This models the keyboard's debouncing as a
 * whole, not a chip's timing (docs/APPROXIMATIONS.md item 34).
 * Everything here is under km_mu: the app thread writes the raw
 * side, the engine thread reads it. */
#define KB_RELEASE_MS   25
#define KB_MIN_PRESS_MS 40
static unsigned char kb_raw[16];        /* what the host last fed */
static long long kb_raw_t[16][8];       /* when the cell's raw value last changed */
static long long kb_vis_t[16][8];       /* when the cell's visible value last changed */

static long long ks_now_ms(void);

void halp8_set_matrix(int idx, unsigned char v)
{
    int bit, diff;
    long long now;
    idx &= 15;
    pthread_mutex_lock(&km_mu);
    now = ks_now_ms();          /* inside the lock: a timestamp sampled
                                 * before it could be arbitrarily old
                                 * by the time the change is published */
    diff = kb_raw[idx] ^ v;
    for (bit = 0; diff && bit < 8; bit++)
        if (diff & (1 << bit)) kb_raw_t[idx][bit] = now;
    kb_raw[idx] = v;
    pthread_mutex_unlock(&km_mu);
}

/* Promote the raw matrix into the guest-visible one (see above). */
static void kb_tick(void)
{
    int row, bit, pull = 0;
    long long now;
    pthread_mutex_lock(&km_mu);
    now = ks_now_ms();          /* inside the lock, as in the writer:
                                 * the ages judged below must be ages
                                 * at promotion time, not at call time */

    /* Pass 1: decide the press promotions. Every press (visible up,
     * raw down; active-low) is promoted this tick, so all that has to
     * be known before the releases are judged is whether there is one:
     * if so, the releases below are pulled through. */
    for (row = 0; row < 16 && !pull; row++)
        if (kmatrix[row] & ~kb_raw[row]) pull = 1;

    /* Pass 2: apply the presses, and the releases that may go. Under
     * the same lock and the same `now`, so a press and a pulled-through
     * release land in one visible snapshot. */
    for (row = 0; row < 16; row++) {
        int diff = kb_raw[row] ^ kmatrix[row];
        if (!diff) continue;
        for (bit = 0; bit < 8; bit++) {
            int m = 1 << bit;
            if (!(diff & m)) continue;
            if (kb_raw[row] & m) {          /* active-low: this is a release */
                /* the minimum visible press is never waived */
                if (now - kb_vis_t[row][bit] < KB_MIN_PRESS_MS) continue;
                /* the stability wait is, when a press goes this tick */
                if (!pull && now - kb_raw_t[row][bit] < KB_RELEASE_MS) continue;
            }
            kmatrix[row] = (unsigned char)((kmatrix[row] & ~m) | (kb_raw[row] & m));
            kb_vis_t[row][bit] = now;
        }
    }
    pthread_mutex_unlock(&km_mu);
}

/* Power-on state: every cell up on both sides, nothing pending. */
static void kb_reset(void)
{
    pthread_mutex_lock(&km_mu);
    memset(kb_raw, 0xFF, sizeof(kb_raw));
    memset(kmatrix, 0xFF, sizeof(kmatrix));   /* idle = nothing pressed */
    memset(kb_raw_t, 0, sizeof(kb_raw_t));
    memset(kb_vis_t, 0, sizeof(kb_vis_t));
    pthread_mutex_unlock(&km_mu);
}
void halp8_request_stop(void) { stop_req = 1; }
int  halp8_stop_requested(void) { return stop_req; }

static volatile int paused_flag = 0;
void halp8_set_paused(int paused) { paused_flag = paused; }
int  halp8_paused(void) { return paused_flag; }

/* ---- keyboard character generation (the ROM's matrix scan) ----
 * The host feeds only the key matrix. On the genuine machine the ROM
 * scans that matrix and turns press edges into characters, with
 * auto-repeat; keyscan() is that scanner. It reads kmatrix (the
 * debounced, guest-visible matrix the key buffer maintains), never
 * writes it, and pushes the generated codes into the key queue the
 * interpreter reads (INKEY$, INPUT, the line editor). It runs on the
 * interpreter thread only (hal_poll / hal_idle / hal_getkey).
 * Matrix layout: QUASI88's published keyport table. Modelled, not
 * genuine (docs/APPROXIMATIONS.md item 32):
 *  - GRPH held: no character is generated at all (the GRPH glyph
 *    layer is not modelled; the matrix bit itself is untouched).
 *  - CAPS / KANA are mechanical locks on the genuine machine: the
 *    matrix bit stays down for the whole lock, and the host app does
 *    the same (its on-screen keyboard latches the cell). The scanner
 *    reads their level from the snapshot, like SHIFT / CTRL.
 *  - CTRL + letter = code 1..26, CTRL+C = break only; CTRL with the
 *    other main-block keys (digits, punctuation) generates nothing.
 *  - Repeat: 500 ms delay, then every 55 ms, the originally generated
 *    code. A repeat is held back while the queue still has unread
 *    keys, so a program that does not read the keyboard cannot bank
 *    up seconds of repeats. */
#define KS_ROWS            12
#define KS_REPEAT_DELAY_MS 500
#define KS_REPEAT_RATE_MS  55
#define KS_NONE  (-1)   /* character key, nothing generated: ends a repeat */
#define KS_INERT (-2)   /* modifier / lock / STOP / unused cell: repeat untouched */

/* Zero ("everything was already down") is a safe power-on value: it
 * yields no press edge on the first scan. */
static unsigned char ks_prev[KS_ROWS];
static int ks_caps = 0, ks_kana = 0;
static int ks_rep_row = -1, ks_rep_bit = 0, ks_rep_code = 0;
static long long ks_rep_due = 0;

/* halfwidth katakana (JIS X 0201) per the JIS key legends */
static const unsigned char ks_kana_letter[26] = {
    0xC1, 0xBA, 0xBF, 0xBC, 0xB2, 0xCA, 0xB7, 0xB8, 0xC6,   /* A-I */
    0xCF, 0xC9, 0xD8, 0xD3, 0xD0, 0xD7, 0xBE, 0xC0, 0xBD,   /* J-R */
    0xC4, 0xB6, 0xC5, 0xCB, 0xC3, 0xBB, 0xDD, 0xC2          /* S-Z */
};
static const unsigned char ks_kana_digit[10] = {
    0xDC, 0xC7, 0xCC, 0xB1, 0xB3, 0xB4, 0xB5, 0xD4, 0xD5, 0xD6
};
static const unsigned char ks_kana_small[10] = {    /* SHIFT; 0 = no small form */
    0, 0, 0, 0xA7, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE
};
static const unsigned char ks_digit_shift[10] = {   /* 0 = none */
    0, '!', '"', '#', '$', '%', '&', '\'', '(', ')'
};
/* punctuation: @ [ YEN ] ^ - (rows 2 and 5), : ; , . / _ (row 7) */
static const unsigned char ks_punct_tab[12][3] = {  /* base, shift, kana; 0 = none */
    { '@', '`', 0xDE }, { '[', '{', 0xDF }, { 0x5C, '|', 0xB0 },
    { ']', '}', 0xD1 }, { '^', '~', 0xCD }, { '-', '=', 0xCE },
    { ':', '*', 0xB9 }, { ';', '+', 0xDA }, { ',', '<', 0xC8 },
    { '.', '>', 0xD9 }, { '/', '?', 0xD2 }, { 0,   '_', 0xDB }
};

static long long ks_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ks_punct(int i, int shift, int ctrl)
{
    int c;
    if (ctrl) return KS_NONE;
    c = ks_punct_tab[i][ks_kana ? 2 : (shift ? 1 : 0)];
    return c ? c : KS_NONE;
}

static int ks_digit(int d, int shift, int ctrl)
{
    if (ctrl) return KS_NONE;
    if (ks_kana)
        return (shift && ks_kana_small[d]) ? ks_kana_small[d] : ks_kana_digit[d];
    if (!shift) return '0' + d;
    return ks_digit_shift[d] ? ks_digit_shift[d] : KS_NONE;
}

/* The code one newly pressed cell generates, with the modifiers read
 * live from the current matrix. Locks and STOP take effect here. */
static int ks_keycode(int row, int bit, const unsigned char *cur)
{
    int shift = !(cur[8] & 0x40), ctrl = !(cur[8] & 0x80);
    int n;
    switch (row) {
    case 0:                             /* keypad 0-7: SHIFT-independent */
        return '0' + bit;
    case 1:                             /* keypad 8 9 * + = , . and RETURN */
        return "89*+=,.\r"[bit];
    case 2: case 3: case 4: case 5:     /* @ A-Z [ YEN ] ^ - */
        n = (row - 2) * 8 + bit;
        if (n >= 1 && n <= 26) {
            if (ctrl) {
                if (n == 3) { if (running) brkflag = 1; return KS_NONE; }    /* CTRL+C */
                return n;
            }
            if (ks_kana) return ks_kana_letter[n - 1];
            /* SHIFT gives lowercase; CAPS inverts that sense */
            return (shift != ks_caps) ? 'a' + n - 1 : 'A' + n - 1;
        }
        return ks_punct(n == 0 ? 0 : n - 26, shift, ctrl);
    case 6:                             /* 0-7 */
        return ks_digit(bit, shift, ctrl);
    case 7:                             /* 8 9 : ; , . / _ */
        if (bit < 2) return ks_digit(8 + bit, shift, ctrl);
        return ks_punct(4 + bit, shift, ctrl);
    case 8:
        switch (bit) {
        case 0: return shift ? KC_SHOME : 0x0B;     /* HOME / CLR */
        case 1: return 0x1E;                        /* up */
        case 2: return 0x1C;                        /* right */
        case 3: return shift ? 0x12 : 0x7F;         /* INS / DEL */
        case 5: return KS_INERT;    /* KANA lock: a level, read in keyscan */
        default: return KS_INERT;                   /* GRPH, SHIFT, CTRL */
        }
    case 9:
        switch (bit) {
        case 0: if (running) brkflag = 1; return KS_INERT;       /* STOP */
        case 6: return ' ';
        case 7: return 0x1B;
        default: return (shift ? KC_SF1 : KC_F1) + bit - 1;     /* F1-F5 */
        }
    case 10:
        switch (bit) {
        case 0: return 0x09;                        /* TAB */
        case 1: return 0x1F;                        /* down */
        case 2: return 0x1D;                        /* left */
        case 3: return KC_HELP;
        case 4: return KC_COPY;
        case 5: return '-';                         /* keypad - */
        case 6: return '/';                         /* keypad / */
        default: return KS_INERT;   /* CAPS lock: a level, read in keyscan */
        }
    case 11:
        if (bit == 0) return KC_ROLLUP;
        if (bit == 1) return KC_ROLLDN;
        return KS_INERT;
    }
    return KS_INERT;
}

static void keyscan(void)
{
    unsigned char cur[KS_ROWS];
    int row, bit;
    pthread_mutex_lock(&km_mu);
    for (row = 0; row < KS_ROWS; row++) cur[row] = kmatrix[row];
    pthread_mutex_unlock(&km_mu);
    if (memcmp(cur, ks_prev, KS_ROWS) != 0) {
        int grph = !(cur[8] & 0x10);
        /* The locking keys are levels, not edges (see the header
         * note): refresh them from every changed snapshot before
         * translating its press edges. */
        ks_kana = !(cur[8] & 0x20);
        ks_caps = !(cur[10] & 0x80);
        for (row = 0; row < KS_ROWS; row++) {
            int edge = ks_prev[row] & ~cur[row] & 0xFF;     /* 1 -> 0 = pressed */
            if (!edge) continue;
            for (bit = 0; bit < 8; bit++) {
                int code;
                if (!(edge & (1 << bit))) continue;
                code = ks_keycode(row, bit, cur);
                if (code == KS_INERT) continue;
                if (code >= 0 && grph) code = KS_NONE;
                if (code >= 0) {
                    halp8_push_key(code);
                    ks_rep_row = row; ks_rep_bit = bit; ks_rep_code = code;
                    ks_rep_due = ks_now_ms() + KS_REPEAT_DELAY_MS;
                } else {
                    ks_rep_row = -1;    /* a new key ends the old repeat */
                }
            }
        }
        memcpy(ks_prev, cur, KS_ROWS);
    }
    if (ks_rep_row >= 0) {
        if (!(cur[8] & 0x10)) {
            ks_rep_row = -1;    /* GRPH held: generation is suppressed */
        } else if (cur[ks_rep_row] & (1 << ks_rep_bit)) {
            ks_rep_row = -1;            /* released */
        } else {
            long long now = ks_now_ms();
            if (now >= ks_rep_due && kq_push_if_empty(ks_rep_code))
                ks_rep_due = now + KS_REPEAT_RATE_MS;
        }
    }
}

static int gon_flag = 1, ton_flag = 1;
static int gfx200 = 0;
static int pal[16][3];
static int pal_init = 0;
static int poll_n = 0;              /* hal_poll's statement counter */
static int scan_n = 0;              /* keyscan's own gate: the keyboard
                                     * scanner keeps its historical
                                     * once-per-64-calls cadence,
                                     * independent of the pacing gate */

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
    kb_reset();                     /* raw and visible: nothing pressed */
    memset(ks_prev, 0xFF, sizeof(ks_prev));     /* scanner: no edges pending */
    ks_caps = ks_kana = 0;
    ks_rep_row = -1; ks_rep_bit = 0; ks_rep_code = 0;
    ks_rep_due = 0;
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
    kb_reset();                     /* idle = nothing pressed */
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
    kb_tick();
    keyscan();
    return kq_pop();
}

int hal_keyhit(void) { return kq_count() > 0; }
int hal_breakflag(void) { int b = brkflag; brkflag = 0; return b; }
unsigned char hal_lastscan(void) { return 0; }
unsigned char hal_keymatrix(int i)
{
    unsigned char v;
    pthread_mutex_lock(&km_mu);
    v = kmatrix[i & 15];
    pthread_mutex_unlock(&km_mu);
    return v;
}
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
    kb_tick();
    keyscan();
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
 * overflows and playback tears. Checked every 8 statements: one
 * check interval advances ~0.004 s (a quarter frame) of emulated
 * time on the statement path (APPROXIMATIONS item 6), ~0.03 s
 * through sector burns, which the 2 s ring absorbs. */
void hal_poll(void)
{
    if (stop_req) p8_bail();
    while (paused_flag && !stop_req) usleep(10000);
    if (stop_req) p8_bail();
    if (++poll_n < 8) return;
    poll_n = 0;
    kb_tick();          /* every gate: INP polling sees promotions promptly */
    if (++scan_n >= 8) {        /* 8 pacing gates = the old 64-call cadence */
        scan_n = 0;
        keyscan();      /* a running program still gets its keys scanned */
    }
    /* Frame-granularity pacing. The audio lead anchors the average
     * rate to wall time; the single threshold below (the 0.10 s target
     * lead plus half a frame) only sets how far production may run
     * ahead before sleeping, i.e. latency. The excess is slept off in
     * chunks of at most 2 ms with the lead re-checked after each. The
     * previous scheme (break at 0.15 s, sleep toward 0.10 s in chunks
     * of up to 50 ms, checked every 64 statements) produced guest
     * frames in ~3-frame bursts separated by long sleeps: measured
     * VRTC wrap arrivals were bimodal (clusters <8 ms apart, then gaps
     * of 40-80 ms, nothing in between), which a 60 Hz display samples
     * as dropped and held frames even though the average is about
     * 60 fps. With the 8-call gate, a check on the statement path tops
     * the lead up by at most a quarter frame (sector burns advance in
     * 16,667-cycle slices, about two frames per eight checks), so in
     * steady playback wraps emerge ~16.7 ms apart in wall time.
     * Wall-clock only: emulated time is untouched. */
    while (!stop_req) {
        const double high = 0.10 + 1.0 / 120.0;     /* target lead + half a frame */
        double ahead = p8_audio_seconds() - p8_wall_seconds();
        double excess;
        long us;
        if (ahead <= high) break;
        excess = (ahead - high) * 1000000.0;
        if (excess > 2000.0) excess = 2000.0;   /* clamp before converting */
        us = (long)excess;
        if (us <= 0) break;
        usleep((useconds_t)us);
    }
    if (stop_req) p8_bail();
}
