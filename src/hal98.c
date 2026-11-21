/* hal98.c - PC-9801 hardware layer (no BIOS calls)
 *
 *  Text VRAM     A000:0000 (codes) / A200:0000 (attributes)
 *  Text GDC      ports 60h (param/status) 62h (command)
 *  Keyboard      8251 at 41h (data) 43h (status) , IRQ1 = INT 09h
 *  VSYNC         IRQ2 = INT 0Ah, re-arm by OUT 64h
 *  Beeper        8255 port C bit3 via 37h (6 = on, 7 = off)
 *  Graphics      planes B:A800 R:B000 G:B800 E:E000, GDC A0h/A2h
 *  Palette       A8h-AEh, 16 color mode via 6Ah
 *  MS-DOS (INT 21h) is used only for files, vectors, memory and clock.
 */
#include "basic.h"
#include <dos.h>
#include <conio.h>
#include <i86.h>

#define TVRAM_SEG 0xA000
#define TATTR_SEG 0xA200

static unsigned short __far *tv = (unsigned short __far *)MK_FP(TVRAM_SEG, 0);
static unsigned char __far *ta = (unsigned char __far *)MK_FP(TATTR_SEG, 0);
static unsigned char __far *bda = (unsigned char __far *)MK_FP(0, 0);

static int w40 = 0;

/* ---------------- keyboard ---------------- */
#define KBUF 64
static volatile int kbuf[KBUF];
static volatile int khead = 0, ktail = 0;
static volatile unsigned char kmat[16];
static volatile unsigned char lastscan = 0;
static volatile unsigned char shift_st = 0;   /* bit0 shift 1 caps 2 kana 3 grph 4 ctrl */
static volatile int brk = 0;
static int inp_direct = 0;

static volatile unsigned long ticks = 0;
static volatile int vsync_seen = 0;

static void (__interrupt __far *old09)(void);
static void (__interrupt __far *old0a)(void);
static unsigned char old_pic_mask;
static int installed = 0;

/* scan code tables (0x00 - 0x33) */
static const char k_norm[0x34] = {
    0x1B,'1','2','3','4','5','6','7','8','9','0','-','^',0x5C,0x08,0x09,
    'q','w','e','r','t','y','u','i','o','p','@','[',0x0D,
    'a','s','d','f','g','h','j','k','l',';',':',']',
    'z','x','c','v','b','n','m',',','.','/','_'
};
static const char k_shift[0x34] = {
    0x1B,'!','"','#','$','%','&','\'','(',')','0','=','~','|',0x08,0x09,
    'Q','W','E','R','T','Y','U','I','O','P','`','{',0x0D,
    'A','S','D','F','G','H','J','K','L','+','*','}',
    'Z','X','C','V','B','N','M','<','>','?','_'
};
static const unsigned char k_kana[0x34] = {
    0x1B,0xC7,0xCC,0xB1,0xB3,0xB4,0xB5,0xD4,0xD5,0xD6,0xDC,0xCE,0xCD,0xB0,0x08,0x09,
    0xC0,0xC3,0xB2,0xBD,0xB6,0xDD,0xC5,0xC6,0xD7,0xBE,0xDE,0xDF,0x0D,
    0xC1,0xC4,0xBC,0xCA,0xB7,0xB8,0xCF,0xC9,0xD8,0xDA,0xB9,0xD1,
    0xC2,0xBB,0xBF,0xCB,0xBA,0xD0,0xD3,0xC8,0xD9,0xD2,0xDB
};
static const unsigned char k_kshift[0x34] = {
    0x1B,0xC7,0xCC,0xA7,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xA6,0xCE,0xCD,0xB0,0x08,0x09,
    0xC0,0xC3,0xA8,0xBD,0xB6,0xDD,0xC5,0xC6,0xD7,0xBE,0xDE,0xA2,0x0D,
    0xC1,0xC4,0xBC,0xCA,0xB7,0xB8,0xCF,0xC9,0xD8,0xDA,0xB9,0xA3,
    0xAF,0xBB,0xBF,0xCB,0xBA,0xD0,0xD3,0xA4,0xA1,0xA5,0xDB
};
/* N88-BASIC(86) style key matrix at INP(&HE0)..INP(&HEC) (bit = 0 while pressed).
 * Layout follows the published key matrix chart; built from our own scan code handling. */
static volatile unsigned char emat[13] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
/* entries: scan code, row (0 = E0h), bit */
static const unsigned char ematmap[][3] = {
    {0x4E,0,0},{0x4A,0,1},{0x4B,0,2},{0x4C,0,3},{0x46,0,4},{0x47,0,5},{0x48,0,6},{0x42,0,7},
    {0x43,1,0},{0x44,1,1},{0x45,1,2},{0x49,1,3},{0x4D,1,4},{0x4F,1,5},{0x50,1,6},{0x1C,1,7},
    {0x1A,2,0},{0x1D,2,1},{0x2D,2,2},{0x2B,2,3},{0x1F,2,4},{0x12,2,5},{0x20,2,6},{0x21,2,7},
    {0x22,3,0},{0x17,3,1},{0x23,3,2},{0x24,3,3},{0x25,3,4},{0x2F,3,5},{0x2E,3,6},{0x18,3,7},
    {0x19,4,0},{0x10,4,1},{0x13,4,2},{0x1E,4,3},{0x14,4,4},{0x16,4,5},{0x2C,4,6},{0x11,4,7},
    {0x2A,5,0},{0x15,5,1},{0x29,5,2},{0x1B,5,3},{0x0D,5,4},{0x28,5,5},{0x0C,5,6},{0x0B,5,7},
    {0x0A,6,0},{0x01,6,1},{0x02,6,2},{0x03,6,3},{0x04,6,4},{0x05,6,5},{0x06,6,6},{0x07,6,7},
    {0x08,7,0},{0x09,7,1},{0x27,7,2},{0x26,7,3},{0x30,7,4},{0x31,7,5},{0x32,7,6},{0x33,7,7},
    {0x3E,8,0},{0x3A,8,1},{0x3C,8,2},{0x39,8,3},{0x73,8,4},{0x72,8,5},{0x70,8,6},{0x74,8,7},
    {0x60,9,0},{0x62,9,1},{0x63,9,2},{0x64,9,3},{0x65,9,4},{0x66,9,5},{0x34,9,6},{0x00,9,7},
    {0x0F,10,0},{0x3D,10,1},{0x3B,10,2},{0x3F,10,3},{0x61,10,4},{0x40,10,5},{0x41,10,6},{0x71,10,7},
    {0x36,11,0},{0x37,11,1},
    {0x0E,12,0},{0x35,12,1},{0x67,12,2},{0x68,12,3},{0x69,12,4},{0x6A,12,5},{0x6B,12,6},{0x38,12,7}
};
static unsigned char emat_index[0x80];   /* scan code -> table index + 1 */

static const char k_tenkey[0x11] = { '-','/','7','8','9','*','4','5','6','+','1','2','3','=','0',',','.' };

static void kpush(int c)
{
    int n = (khead + 1) % KBUF;
    if (n == ktail) return;
    kbuf[khead] = c;
    khead = n;
}

static void key_event(unsigned char sc)
{
    unsigned char code = sc & 0x7F;
    int make = !(sc & 0x80);
    unsigned char bit = (unsigned char)(1 << (code & 7));
    int idx = code >> 3;
    lastscan = sc;
    if (make) kmat[idx] |= bit; else kmat[idx] &= (unsigned char)~bit;
    bda[0x52A + idx] = kmat[idx];             /* keep the classic key status table */
    if (emat_index[code]) {
        const unsigned char *m = ematmap[emat_index[code] - 1];
        if (make) emat[m[1]] &= (unsigned char)~(1 << m[2]);
        else emat[m[1]] |= (unsigned char)(1 << m[2]);
    }
    switch (code) {
    case 0x70: if (make) shift_st |= 1; else shift_st &= ~1; break;
    case 0x71: if (make) shift_st |= 2; else shift_st &= ~2; break;
    case 0x72: if (make) shift_st |= 4; else shift_st &= ~4; break;
    case 0x73: if (make) shift_st |= 8; else shift_st &= ~8; break;
    case 0x74: if (make) shift_st |= 16; else shift_st &= ~16; break;
    }
    bda[0x53A] = shift_st;
    if (!make || code >= 0x70) return;
    if (code == 0x60) { brk = 1; return; }          /* STOP */
    if (code < 0x34) {
        int c;
        if (shift_st & 16) {                        /* CTRL */
            c = (unsigned char)k_shift[code];
            if (c >= 0x40 && c <= 0x7F) c &= 0x1F;
            else if (code == 0x0D) c = 0x1C;
            else if (code == 0x1C) c = 0x0D;
            else return;
            if (c == 3) { brk = 1; return; }
            kpush(c);
            return;
        }
        if (shift_st & 4) c = (shift_st & 1) ? k_kshift[code] : k_kana[code];
        else {
            c = (shift_st & 1) ? (unsigned char)k_shift[code] : (unsigned char)k_norm[code];
            if ((shift_st & 2) && c >= 'a' && c <= 'z') c -= 32;
            else if ((shift_st & 2) && c >= 'A' && c <= 'Z' && (shift_st & 1)) c += 32;
        }
        kpush(c);
        return;
    }
    switch (code) {
    case 0x34: kpush(' '); return;
    case 0x36: kpush(KC_ROLLUP); return;
    case 0x37: kpush(KC_ROLLDN); return;
    case 0x38: kpush(0x12); return;
    case 0x39: kpush(0x7F); return;
    case 0x3A: kpush(0x1E); return;
    case 0x3B: kpush(0x1D); return;
    case 0x3C: kpush(0x1C); return;
    case 0x3D: kpush(0x1F); return;
    case 0x3E: kpush((shift_st & 1) ? KC_SHOME : 0x0B); return;
    case 0x3F: kpush(KC_HELP); return;
    case 0x61: kpush(KC_COPY); return;
    }
    if (code >= 0x40 && code <= 0x50) { kpush((unsigned char)k_tenkey[code - 0x40]); return; }
    if (code >= 0x62 && code <= 0x6B) { kpush(((shift_st & 1) ? KC_SF1 : KC_F1) + (code - 0x62)); return; }
}

static void __interrupt __far kbd_isr(void)
{
    unsigned char st, sc;
    st = (unsigned char)inp(0x43);
    sc = (unsigned char)inp(0x41);
    if (st & 0x38) outp(0x43, 0x16);   /* clear error flags */
    outp(0x00, 0x20);                  /* EOI master PIC */
    key_event(sc);
}

static void __interrupt __far vsync_isr(void)
{
    ticks++;
    vsync_seen = 1;
    outp(0x64, 0);                     /* re-arm CRTV interrupt */
    outp(0x00, 0x20);
}

/* ---------------- GDC ---------------- */
static void gdc_wait(void)
{
    unsigned n = 20000;
    while ((inp(0x60) & 0x02) && --n) {}
}
static void gdc_cmd(int c) { gdc_wait(); outp(0x62, c); }
static void gdc_prm(int p) { gdc_wait(); outp(0x60, p); }

static int cur_vis = -1, cur_ins = -1;
static unsigned cur_addr = 0xFFFF;

void hal_cursor(int x, int y, int visible, int insmode)
{
    unsigned addr = (unsigned)(y * 80 + (w40 ? x * 2 : x));
    if (visible != cur_vis || insmode != cur_ins) {
        int top = insmode ? 8 : 0, bot = 15;
        int br = 12;  /* blink rate */
        gdc_cmd(0x4B);
        gdc_prm((visible ? 0x80 : 0x00) | 0x0F);
        gdc_prm(((br & 3) << 6) | top);
        gdc_prm((bot << 3) | ((br >> 2) & 7));
        cur_vis = visible; cur_ins = insmode;
    }
    if (visible && addr != cur_addr) {
        gdc_cmd(0x49);
        gdc_prm(addr & 0xFF);
        gdc_prm((addr >> 8) & 0xFF);
        gdc_prm(0);
        cur_addr = addr;
    }
}

/* ---------------- text ---------------- */
void hal_tput(int x, int y, unsigned short v, unsigned char a)
{
    unsigned i = (unsigned)(y * 80 + (w40 ? x * 2 : x));
    tv[i] = v;
    ta[i * 2] = a;
}

void hal_trow(int y, const unsigned short *codes, const unsigned char *attrs, int n)
{
    unsigned i = (unsigned)(y * 80);
    int x;
    if (w40) {
        for (x = 0; x < n; x++) { tv[i + x * 2] = codes[x]; ta[(i + x * 2) * 2] = attrs[x]; }
    } else {
        unsigned short __far *p = tv + i;
        unsigned char __far *q = ta + i * 2;
        for (x = 0; x < n; x++) { p[x] = codes[x]; q[x * 2] = attrs[x]; }
    }
}

void hal_gfx200(int on) { (void)on; }

void hal_width40(int on)
{
    w40 = on;
    outp(0x68, on ? 0x05 : 0x04);
}

static void bios_kbuf_clear(void);
/* ---------------- init / term ---------------- */
void hal_init(int flags)
{
    (void)flags;
    memset((void *)kmat, 0, sizeof(kmat));
    {
        unsigned i;
        for (i = 0; i < sizeof(ematmap) / sizeof(ematmap[0]); i++) emat_index[ematmap[i][0]] = (unsigned char)(i + 1);
    }
    bios_kbuf_clear();
    old09 = _dos_getvect(0x09);
    old0a = _dos_getvect(0x0A);
    _disable();
    _dos_setvect(0x09, kbd_isr);
    _dos_setvect(0x0A, vsync_isr);
    old_pic_mask = (unsigned char)inp(0x02);
    outp(0x02, old_pic_mask & ~0x06);       /* enable IRQ1 + IRQ2 */
    _enable();
    outp(0x64, 0);
    installed = 1;
    outp(0x68, 0x04);                         /* 80 columns */
    outp(0x68, 0x08);                         /* 400 line graphics */
    hal_cursor(0, 0, 0, 0);
}

void hal_term(void)
{
    int i;
    static const char cls[] = "\x1b[2J$";
    union REGS r;
    struct SREGS s;
    if (!installed) return;
    hal_beep(0);
    _disable();
    outp(0x02, old_pic_mask);
    _dos_setvect(0x09, old09);
    _dos_setvect(0x0A, old0a);
    _enable();
    installed = 0;
    if (w40) hal_width40(0);
    for (i = 0; i < 80 * 25; i++) { tv[i] = ' '; ta[i * 2] = 0xE1; }
    /* clear graphics */
    hal_gpage(0, 0);
    hal_gclear(0);
    hal_cursor(0, 0, 1, 0);
    /* let MS-DOS clear its console and home its cursor */
    segread(&s);
    s.ds = FP_SEG(cls);
    r.h.ah = 0x09;
    r.x.dx = FP_OFF(cls);
    intdosx(&r, &r, &s);
}

/* ---------------- keyboard API ---------------- */
/* Japanese input from the emulator's IME is placed in the BIOS key buffer
 * (0000:0502h-0521h, 16 entries of 2 bytes; 0524h head, 0526h tail, 0528h count).
 * Low byte = character code (Shift_JIS byte), high byte = 0. */
#define BKB_HEAD  (*(unsigned short __far *)MK_FP(0, 0x524))
#define BKB_TAIL  (*(unsigned short __far *)MK_FP(0, 0x526))
#define BKB_COUNT (*(unsigned char __far *)MK_FP(0, 0x528))

static void bios_kbuf_clear(void)
{
    _disable();
    BKB_HEAD = 0x502; BKB_TAIL = 0x502; BKB_COUNT = 0;
    _enable();
}

static int bios_kbuf_get(void)
{
    int c = -1;
    _disable();
    if (BKB_COUNT != 0) {
        unsigned h = BKB_HEAD;
        if (h < 0x502 || h > 0x520 || (h & 1)) h = 0x502;   /* guard against a broken pointer */
        c = *(unsigned char __far *)MK_FP(0, h);
        h += 2;
        if (h >= 0x522) h = 0x502;
        BKB_HEAD = (unsigned short)h;
        BKB_COUNT = (unsigned char)(BKB_COUNT - 1);
    }
    _enable();
    return c;
}

int hal_getkey(void)
{
    int c = bios_kbuf_get();          /* IME text first: it was typed before later keys */
    if (c >= 0) return c;
    if (khead == ktail) return -1;
    _disable();
    c = kbuf[ktail];
    ktail = (ktail + 1) % KBUF;
    _enable();
    return c;
}

int hal_keyhit(void) { return khead != ktail || BKB_COUNT != 0; }
int hal_breakflag(void) { int b = brk; brk = 0; return b; }
unsigned char hal_lastscan(void) { return lastscan; }
unsigned char hal_keymatrix(int idx) { return kmat[idx & 15]; }
int hal_shiftstate(void) { return shift_st; }
void hal_set_inp_direct(int on) { inp_direct = on; }
int hal_inp_direct(void) { return inp_direct; }

/* ---------------- time ---------------- */
unsigned long hal_ticks(void)
{
    static int last = 0;
    unsigned long t;
    if (!vsync_seen) {
        /* fallback: poll GDC VSYNC status bit */
        int s = inp(0x60) & 0x20;
        if (s && !last) ticks++;
        last = s;
    }
    _disable(); t = ticks; _enable();
    return t;
}

void hal_beep(int on) { outp(0x37, on ? 0x06 : 0x07); }
void hal_idle(void) { }
void hal_poll(void) { }

/* ---------------- ports / memory ---------------- */
int hal_inp(unsigned port)
{
    if (!inp_direct && port == 0x41) return lastscan;
    if (port >= 0xE0 && port <= 0xEC) return emat[port - 0xE0];   /* key matrix */
    return inp(port);
}
void hal_outp(unsigned port, int val) { outp(port, val); }
int hal_peek(unsigned seg, unsigned off) { return *(unsigned char __far *)MK_FP(seg, off); }
void hal_poke(unsigned seg, unsigned off, int val) { *(unsigned char __far *)MK_FP(seg, off) = (unsigned char)val; }

static int dseg_anchor;
unsigned hal_dataseg(void) { return FP_SEG(&dseg_anchor); }
unsigned long hal_varptr(void *p) { return ((unsigned long)FP_SEG(p) << 16) | FP_OFF(p); }

void hal_call(unsigned seg, unsigned off)
{
    void (__far *fp)(void) = (void (__far *)(void))MK_FP(seg, off);
    fp();
}

/* ---------------- graphics ---------------- */
static const unsigned plane_seg[4] = { 0xA800, 0xB000, 0xB800, 0xE000 };

void hal_gon(int on) { outp(0xA2, on ? 0x0D : 0x0C); }
void hal_ton(int on) { (void)on; }
void hal_pile_reset(void) { }
void hal_text_clear_region(int top, int bot) { (void)top; (void)bot; }
void hal_gpage(int active, int display) { outp(0xA6, active & 1); outp(0xA4, display & 1); }
void hal_analog(int on) { outp(0x6A, on ? 0x01 : 0x00); }
void hal_palette(int idx, int g, int r, int b)
{
    outp(0xA8, idx); outp(0xAA, g); outp(0xAC, r); outp(0xAE, b);
}

void hal_gpset(int x, int y, int c)
{
    unsigned off;
    unsigned char bit;
    int p;
    if (x < 0 || x > 639 || y < 0 || y > 399) return;
    off = (unsigned)y * 80 + (x >> 3);
    bit = (unsigned char)(0x80 >> (x & 7));
    for (p = 0; p < 4; p++) {
        unsigned char __far *q = (unsigned char __far *)MK_FP(plane_seg[p], off);
        if (c & (1 << p)) *q |= bit; else *q &= (unsigned char)~bit;
    }
}

int hal_gpoint(int x, int y)
{
    unsigned off;
    unsigned char bit;
    int p, c = 0;
    if (x < 0 || x > 639 || y < 0 || y > 399) return -1;
    off = (unsigned)y * 80 + (x >> 3);
    bit = (unsigned char)(0x80 >> (x & 7));
    for (p = 0; p < 4; p++)
        if (*(unsigned char __far *)MK_FP(plane_seg[p], off) & bit) c |= 1 << p;
    return c;
}

void hal_ghline(int x1, int x2, int y, int c)
{
    int p;
    unsigned base;
    int b1, b2;
    unsigned char m1, m2;
    if (y < 0 || y > 399) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (x1 < 0) x1 = 0;
    if (x2 > 639) x2 = 639;
    if (x1 > x2) return;
    base = (unsigned)y * 80;
    b1 = x1 >> 3; b2 = x2 >> 3;
    m1 = (unsigned char)(0xFF >> (x1 & 7));
    m2 = (unsigned char)(0xFF << (7 - (x2 & 7)));
    for (p = 0; p < 4; p++) {
        unsigned char __far *q = (unsigned char __far *)MK_FP(plane_seg[p], base);
        int on = c & (1 << p);
        if (b1 == b2) {
            unsigned char m = m1 & m2;
            if (on) q[b1] |= m; else q[b1] &= (unsigned char)~m;
        } else {
            int b;
            if (on) q[b1] |= m1; else q[b1] &= (unsigned char)~m1;
            for (b = b1 + 1; b < b2; b++) q[b] = on ? 0xFF : 0x00;
            if (on) q[b2] |= m2; else q[b2] &= (unsigned char)~m2;
        }
    }
}

void hal_gclear(int c)
{
    int p;
    for (p = 0; p < 4; p++)
        _fmemset(MK_FP(plane_seg[p], 0), (c & (1 << p)) ? 0xFF : 0x00, 32000);
}

/* ---------------- date / time via MS-DOS ---------------- */
void hal_gettime(int *h, int *m, int *s)
{
    struct dostime_t t;
    _dos_gettime(&t);
    *h = t.hour; *m = t.minute; *s = t.second;
}
void hal_getdate(int *y, int *mo, int *d)
{
    struct dosdate_t t;
    _dos_getdate(&t);
    *y = t.year; *mo = t.month; *d = t.day;
}
void hal_settime(int h, int m, int s)
{
    struct dostime_t t;
    t.hour = (unsigned char)h; t.minute = (unsigned char)m; t.second = (unsigned char)s; t.hsecond = 0;
    _dos_settime(&t);
}
void hal_setdate(int y, int mo, int d)
{
    struct dosdate_t t;
    t.year = (unsigned short)y; t.month = (unsigned char)mo; t.day = (unsigned char)d;
    _dos_setdate(&t);
}
