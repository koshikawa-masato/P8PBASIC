/* vessel.h - Z80 "vessel": runs BLOAD ,R machine code on an emulated Z80
 *   The Z80 shares the interpreter's 64KB guest memory (hal_peek/hal_poke).
 *   CPU core: third_party/z80 (superzazu/z80, MIT).
 */
#ifndef VESSEL_H
#define VESSEL_H

#ifdef __cplusplus
extern "C" {
#endif

void vessel_init(void);                            /* idempotent one-time setup */
void vessel_session_reset(void);                   /* full power-on reset + setup for a new session in the same process; call after halp8_session_reset() */
void vessel_forget_disk(void);                     /* drop the open D88 image; the next read re-opens the current P8P_DISK (engine.c p8_create) */
void vessel_exec(unsigned entry);                 /* run Z80 from entry until it RETs to a sentinel */
long vessel_call(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de);
long vessel_call_pushed(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de, unsigned pushed); /* call with preset registers */
int  vessel_d88_sector(int t, int r, unsigned char *dst); /* raw D88 sector (t = cyl*2+head) */
const unsigned char *vessel_vram_plane(int bank); /* graphics VRAM planes 0=B,1=R,2=G; 80x200 bytes each */
void vessel_vram_pset(int x, int y, int c);   /* interpreter write-through; y in 400-line frame coordinates */
void vessel_vram_clear(int c);                /* interpreter write-through: fill the visible 80x200 bytes of all planes with color c */
int  vessel_vram_gpoint(int x, int y);        /* 3-plane color index 0..7 at frame coordinates; 0 when out of range */
void vessel_palette(int idx, int *r, int *g, int *b); /* palette registers 0..7, out 0..255 */
void vessel_set_palette(int idx, int r7, int g7, int b7); /* components 0..7 */
int  vessel_disk_read(int img, int ltrack, int sector, int count, unsigned addr);
int  vessel_dram_load(int img, int ltrack, int tracks); /* FFDE services 0/1: raw sector reads from P8P_DISK (img = bundled disk image index) */
void vessel_notify_load(unsigned addr, long len);  /* called after BLOAD loads a block */
int  vessel_relay_target(unsigned vector, const char *name, unsigned *target); /* extension-statement handler from the EEA vector's JP (0xEEA1/4/7); 0 = not hooked (warns once per vector per session) */
void vessel_prime_arg(long v);  /* plant the first-arg descriptor (EC41/EC44) as the ROM dispatcher would */
unsigned long vessel_frame_count(void);   /* VRTC period wraps since session reset (the guest frame clock) */
void vessel_pump(void);                            /* advance emulated time by one full pump slice + deliver due interrupts (bulk waits inside one statement) */
void vessel_statement(void);                       /* advance emulated time by one BASIC statement's modelled cost + deliver due interrupts (exec_loop, once per pass) */
unsigned long vessel_opn_keyons(void);             /* OPNA key on/off write count (diagnostics) */
unsigned long vessel_opn_writes(void);             /* OPNA bank-0 write total (diagnostics) */
unsigned long vessel_opn_writes1(void);            /* OPNA bank-1 write total (diagnostics) */
unsigned long vessel_opn_adpcmb_ctl(void);         /* ADPCM-B control writes (diagnostics) */
unsigned long vessel_opn_class(int lo, int hi);    /* bank-0 write count over a register range */
unsigned long vessel_opn_rhythm_writes(void);      /* ADPCM-A block writes (diagnostics) */

/* Diagnostic counters frozen at a fixed point of EMULATED time, for
 * exact regression assertions (tools/embed_test.c): the emulated
 * clock starts with the session's first machine-code use and does
 * not depend on host scheduling, unlike a sample taken by the host
 * thread. vessel_diag_snapshot_at() arms the snapshot (emulated
 * seconds, 0 = off; the setting survives vessel_session_reset);
 * vessel_diag_snapshot() returns 1 and fills *out once it was taken.
 * Read it from another thread only after the engine thread stopped. */
typedef struct {
    unsigned long tb_irqs;      /* Timer B interrupts queued with the CPU */
    unsigned long vrtc_irqs;    /* VRTC interrupts queued with the CPU */
    unsigned long b0_ssg;       /* bank-0 writes, registers 00-0F */
    unsigned long b0_rhythm;    /* bank-0 writes, 10-1F (rhythm / ADPCM-A) */
    unsigned long b0_mode;      /* bank-0 writes, 20-2F (timers, mode, key on/off) */
    unsigned long b0_fm;        /* bank-0 writes, 30-FF (FM channels 1-3) */
    unsigned long b0_keyon;     /* bank-0 writes to 28 (key on/off) */
    unsigned long b0_tb;        /* bank-0 writes to 26 (Timer B = tempo) */
    unsigned long b1_adpcm;     /* bank-1 writes, 00-2F (ADPCM-B block) */
    unsigned long b1_fm;        /* bank-1 writes, 30-FF (FM channels 4-6) */
    unsigned long adpcm_b_reads; /* ADPCM-B DRAM bytes the chip fetched (as rendered so far) */
} VesselDiag;
void vessel_diag_snapshot_at(long emu_seconds);
int  vessel_diag_snapshot(VesselDiag *out);

/* The PC-88 port model, shared by the Z80 and by BASIC's own INP/OUT
 * (the host HAL delegates hal_inp/hal_outp to these). */
int  vessel_port_in(int port);
void vessel_port_out(int port, int value);

/* Screen overlay ("pile") register, port 0x53: bit0 hides the TEXT
 * plane, bits 0x02/0x04/0x08 hide the B/R/G graphics planes. */
int  vessel_text_hidden(void);      /* 1 while port 0x53 bit0 is set */
int  vessel_pile_plane_mask(void);  /* hidden planes as frame colour-index bits (B=1, R=2, G=4) */
void vessel_pile_reset(void);       /* port 0x53 := 0 (the SCREEN statement) */

#ifdef __cplusplus
}
#endif

#endif
