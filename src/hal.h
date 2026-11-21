/* hal.h - hardware abstraction layer for P9PBASIC
 *   PC-98 implementation : hal98.c  (direct hardware access, no BIOS)
 *   Host test implementation : halhost.c
 */
#ifndef HAL_H
#define HAL_H

/* ---- text screen (physical 80x25 cells) ---- */
void hal_init(int argc_flags);
void hal_term(void);
void hal_tput(int x, int y, unsigned short vcode, unsigned char attr); /* raw VRAM word */
void hal_cursor(int x, int y, int visible, int insmode);
void hal_width40(int on);
void hal_gfx200(int on);        /* 1 = interpreter is in a 200-line SCREEN mode */
void hal_trow(int y, const unsigned short *codes, const unsigned char *attrs, int n);

/* ---- keyboard ---- */
/* returns -1 if none. 0..255 = character, >=0x100 special (see KC_*) */
int  hal_getkey(void);
int  hal_keyhit(void);
int  hal_breakflag(void);       /* returns and clears STOP / CTRL+C flag */
unsigned char hal_lastscan(void);
unsigned char hal_keymatrix(int idx);  /* 16 bytes */
int  hal_shiftstate(void);
void hal_set_inp_direct(int on);
int  hal_inp_direct(void);

#define KC_F1    0x101   /* .. KC_F1+9 */
#define KC_SF1   0x111   /* shift+f1 .. */
#define KC_HELP  0x120
#define KC_ROLLUP 0x121
#define KC_ROLLDN 0x122
#define KC_COPY  0x123
#define KC_SHOME 0x124   /* shift+HOME = CLR */

/* ---- time ---- */
unsigned long hal_ticks(void);  /* ~ 1/60 sec units */
void hal_beep(int on);
void hal_idle(void);
void hal_poll(void);            /* called once per statement; embedded
                                   hosts use it to stop the engine */

/* ---- ports / memory ---- */
int  hal_inp(unsigned port);
void hal_outp(unsigned port, int val);
int  hal_peek(unsigned seg, unsigned off);
void hal_poke(unsigned seg, unsigned off, int val);
unsigned hal_dataseg(void);
unsigned long hal_varptr(void *p);  /* seg:off packed (seg<<16|off) */
void hal_call(unsigned seg, unsigned off);

/* ---- graphics (640x400, 4 planes) ---- */
void hal_gon(int on);
void hal_ton(int on);
void hal_pile_reset(void);      /* SCREEN: overlay register (PC-88 port 0x53) back to 0 */
void hal_text_clear_region(int top, int bot);   /* CLS: blank guest text image rows top..bot (PC-88 0xF3C8); no-op elsewhere */
void hal_gpage(int active, int display);
void hal_gpset(int x, int y, int c);
int  hal_gpoint(int x, int y);
void hal_ghline(int x1, int x2, int y, int c);
void hal_gclear(int c);
void hal_palette(int idx, int g, int r, int b);  /* 0..15 each */
void hal_analog(int on);

/* ---- date/time ---- */
void hal_gettime(int *h, int *m, int *s);
void hal_getdate(int *y, int *mo, int *d);
void hal_settime(int h, int m, int s);
void hal_setdate(int y, int mo, int d);

#endif
