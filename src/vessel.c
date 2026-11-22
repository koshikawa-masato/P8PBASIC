/* vessel.c - Z80 "vessel": runs BLOAD ,R machine code on an emulated Z80
 *   Memory is the interpreter's 64KB guest memory (hal_peek/hal_poke, seg 0).
 *   PC-88 ROM entry points are trapped by address and emulated or skipped.
 *   I/O ports model the PC-88 disk subsystem (8255 at FCh-FFh, D88 image
 *   from P8P_DISK), the OPNA register file with its Timer B, and the
 *   IM 2 interrupt pair (VRTC + Timer B) that PMD's playback runs on.
 *   Trace output (stderr) is enabled by the P8P_TRACE_VESSEL environment variable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal.h"
#include "vessel.h"
#include "opna.h"
#include "z80.h"

#define STEP_CAP    4000000L
/* The call stack must live BELOW the PC-88 VRAM window (0xC000-0xFFFF):
 * guest code banks that window to the VRAM planes / ALU during draws,
 * which would redirect stack pushes and pops (and the sentinel) into
 * VRAM. The genuine interpreter's stack is in its own RAM for the same
 * reason. (Found 2026-11-18: with the stack at 0xF000, XDD 8's
 * decompressor returned through VRAM bytes and slid away.)
 * The stack therefore lives at 0x98F0: below the window, in the gap
 * the programs themselves reserve — CLEAR ,&H97FF caps BASIC at
 * 0x97FF and NEOMSC loads at 0x9900, leaving 0x9800-0x98FF unused.
 * NOT inside a driver image: an earlier placement at 0xBF00 sat
 * inside PMD's loaded span (0xAA00-0xC9FF, 32 sectors), and stack
 * writes there silently corrupted the driver — playback rendered
 * as digital silence in the engine while code inspection showed
 * nothing wrong (found 2026-11-18 by bisecting the embed build
 * against the pre-banking tree). */
#define STACK_INIT  0x98F0u
#define SENTINEL    0xFFFFu
#define PMD_START   0xAA00u
#define PMD_END     0xAFFFu

static z80 cpu;
static int inited = 0;
static int pump_live = 0;   /* set by the session's first vessel_init() call */
static int vtrace = -1;
static int pmd_loaded = 0;

/* NEOMSC detection (docs/APPROXIMATIONS.md item 25). The traps that
 * are only valid for NEOMSC and its bundled PMD - the 0xA794 argument
 * fetch and the "PMD not loaded yet" RET-only range - are armed only
 * while neomsc_loaded is set; any other title's (or a user's own)
 * machine code at those addresses runs as written.
 *
 * NEOMSC counts as present when a load was announced
 * (vessel_notify_load) that covers exactly NEOMSC's BSAVE header
 * range, 0x9900-0xA95E, and the body's first bytes in guest memory
 * equal neomsc_sig. Only BLOAD can announce that range: the other
 * announcer, vessel_disk_read, loads whole 256-byte sectors and
 * 0x105F is not a multiple of 256.
 *
 * The signature is the first 30 bytes of the body on the MA2026 disk
 * (the file's bytes 4..33), which are NEOMSC's whole hook
 * installation:
 *   F3          DI
 *   3E C3       LD A,0C3h            ; JP opcode
 *   32 A1 EE    LD (0EEA1h),A        ; FFDD vector
 *   32 A4 EE    LD (0EEA4h),A        ; FFDE vector
 *   32 A7 EE    LD (0EEA7h),A        ; FFDF vector
 *   21 4D 99    LD HL,0994Dh         ; FFDE handler
 *   22 A5 EE    LD (0EEA5h),HL
 *   21 5C 9E    LD HL,09E5Ch         ; FFDD handler
 *   22 A2 EE    LD (0EEA2h),HL
 *   21 DB A6    LD HL,0A6DBh         ; FFDF handler
 *   22 A8 EE    LD (0EEA8h),HL
 * It is discriminating because it carries the three handler addresses
 * as absolute operands: a body that matches plants exactly the
 * handlers of docs/NEOMSC-CONTRACT.md, which is the layout the gated
 * traps assume (0xA794 is an address inside that layout). Other code
 * hooking the same three vectors would have to place its handlers at
 * the same three addresses, in the same order, in a body of the same
 * length, to match.
 *
 * The predicate fingerprints the BLOAD operation, not the memory
 * contents: every announced load that overlaps the body range
 * re-evaluates it (vessel_notify_load) - the matching load sets the
 * flag, any other overlapping load clears it, since the body is no
 * longer intact. Loads elsewhere leave it alone, and so do writes
 * that are never announced (POKE, guest code): a body assembled or
 * patched that way is not tracked. vessel_session_reset clears it. */
#define NEOMSC_START 0x9900u
#define NEOMSC_END   0xA95Eu
static const unsigned char neomsc_sig[30] = {
    0xF3, 0x3E, 0xC3, 0x32, 0xA1, 0xEE, 0x32, 0xA4, 0xEE, 0x32,
    0xA7, 0xEE, 0x21, 0x4D, 0x99, 0x22, 0xA5, 0xEE, 0x21, 0x5C,
    0x9E, 0x22, 0xA2, 0xEE, 0x21, 0xDB, 0xA6, 0x22, 0xA8, 0xEE
};
static int neomsc_loaded = 0;
static unsigned char relay_warned = 0;  /* bit per EEA vector, see vessel_relay_target */

static unsigned char rom_seen[0x8000 / 8];
static unsigned char pmd_seen[(PMD_END - PMD_START + 1 + 7) / 8];
static unsigned char in_seen[256 / 8], out_seen[256 / 8];

static int tracing(void)
{
    if (vtrace < 0) vtrace = getenv("P8P_TRACE_VESSEL") != 0;
    return vtrace;
}

/* P8P_TRACE_SP, read once (the run loop samples it once per call). */
static int trace_sp(void)
{
    static int v = -1;
    if (v < 0) v = getenv("P8P_TRACE_SP") != 0;
    return v;
}

/* returns 1 the first time bit n of map is tested */
static int first_time(unsigned char *map, unsigned n)
{
    if (map[n >> 3] & (1 << (n & 7))) return 0;
    map[n >> 3] |= (unsigned char)(1 << (n & 7));
    return 1;
}

static unsigned rd16(unsigned a)
{
    return (unsigned)hal_peek(0, a & 0xFFFF) | ((unsigned)hal_peek(0, (a + 1) & 0xFFFF) << 8);
}

static void trace_regs(void)
{
    fprintf(stderr, " A=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X\n",
        cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l, (unsigned)cpu.sp);
}

/* ================= disk image (D88) ================= */

#define D88_MAX 0x200000L
static unsigned char d88[D88_MAX];
static long d88_size = 0;
static int d88_state = 0;   /* 0 = not tried, 1 = ok, -1 = unavailable */

/* A .D88 file can bundle several disk images (a multi-disk set);
 * each image's header carries its total size at +0x1C. NEOMSC's
 * XDE services take the image index as their second argument
 * (MA2026: image 0 = "MusicArena2026 A", image 1 = "... B" — the
 * song data lives on B, everything else on A). Bases are recorded
 * at open; an index the file does not hold is a read failure
 * (d88_base), never a fallback to image 0: the fallback played
 * image 0's bytes as a song on an A-only file. */
#define D88_MAX_IMAGES 8
static long d88_image_base[D88_MAX_IMAGES];
static int d88_image_count = 0;
static int xde_img_warned = 0;  /* see xde_image_ok */

static int d88_open(void)
{
    const char *fn;
    FILE *fp;
    if (d88_state) return d88_state > 0;
    d88_state = -1;
    fn = getenv("P8P_DISK");
    if (!fn) return 0;
    fp = fopen(fn, "rb");
    if (!fp) { if (tracing()) fprintf(stderr, "SUBSYS cannot open %s\n", fn); return 0; }
    d88_size = (long)fread(d88, 1, (size_t)D88_MAX, fp);
    /* A file that does not fit the buffer is refused, not cut short:
     * the images and sectors past the cut would silently be missing.
     * Reported even with tracing off - every later read fails. */
    if (d88_size == D88_MAX && fgetc(fp) != EOF) {
        fclose(fp);
        fprintf(stderr, "VESSEL disk %s is larger than %ld bytes,"
                " not loaded\n", fn, D88_MAX);
        d88_size = 0;
        return 0;
    }
    fclose(fp);
    if (d88_size < 0x20 + 164 * 4) return 0;
    d88_image_count = 0;
    {
        long off = 0;
        while (off + 0x20 + 164 * 4 <= d88_size &&
               d88_image_count < D88_MAX_IMAGES) {
            long isz = (long)((unsigned long)d88[off + 0x1C] |
                       ((unsigned long)d88[off + 0x1D] << 8) |
                       ((unsigned long)d88[off + 0x1E] << 16) |
                       ((unsigned long)d88[off + 0x1F] << 24));
            d88_image_base[d88_image_count++] = off;
            if (isz < 0x20 + 164 * 4 || off + isz > d88_size) break;
            off += isz;
        }
    }
    if (d88_image_count == 0) { d88_image_base[0] = 0; d88_image_count = 1; }
    d88_state = 1;
    return 1;
}

/* Drop the open disk image, so the next read opens the current
 * P8P_DISK afresh (this also discards the in-memory sector writes
 * made to the old image). Called by engine.c p8_create(): a new
 * engine binds a new disk, and p8_boot's materialization already
 * reads sectors through d88_open(), before the session reset. */
void vessel_forget_disk(void)
{
    d88_state = 0; d88_size = 0; d88_image_count = 0;
}

/* file offset of disk image `img`, or -1 when the open file holds no
 * such image (the caller fails the read / write) */
static long d88_base(int img)
{
    if (img < 0 || img >= d88_image_count) return -1;
    return d88_image_base[img];
}

static unsigned d88_u16(long p)
{
    return (unsigned)d88[p] | ((unsigned)d88[p + 1] << 8);
}

static unsigned long d88_u32(long p)
{
    return (unsigned long)d88_u16(p) | ((unsigned long)d88_u16(p + 2) << 16);
}

/* copy sector r of subsystem track t (256 bytes) of disk image
 * `img` to dst; 0 on error */
static int d88_read_sector_img(int img, int t, int r, unsigned char *dst)
{
    int cyl, head, n, count;
    unsigned long block;
    long pos, base;
    unsigned dsize, cp;
    if (!d88_open()) return 0;
    if (t < 0 || t >= 164) return 0;
    base = d88_base(img);
    if (base < 0) return 0;
    cyl = t >> 1; head = t & 1;
    block = d88_u32(base + 0x20 + (long)(cyl * 2 + head) * 4);
    if (block == 0 || base + (long)block + 16 > d88_size) return 0;
    pos = base + (long)block;
    count = (int)d88_u16(pos + 4);   /* sectors in this track */
    if (count <= 0 || count > 64) count = 64;
    for (n = 0; n < count && pos + 16 <= d88_size; n++) {
        dsize = d88_u16(pos + 0x0E);
        if (dsize == 0) dsize = 128u << (d88[pos + 3] & 7);
        if (d88[pos + 2] == r) {
            cp = dsize < 256 ? dsize : 256;
            if (pos + 16 + (long)cp > d88_size) return 0;
            memset(dst, 0, 256);
            memcpy(dst, d88 + pos + 16, cp);
            return 1;
        }
        pos += 16 + (long)dsize;
    }
    return 0;
}

/* write 256 bytes from src into sector r of subsystem track t of
 * disk image `img`, in the in-memory image only: the file on disk
 * is never modified, so a guest write behaves like a floppy in the
 * drive for the session and every reader of the image (subsystem,
 * file layer, XDE services) sees it. 0 on error. */
static int d88_write_sector_img(int img, int t, int r, const unsigned char *src)
{
    int cyl, head, n, count;
    unsigned long block;
    long pos, base;
    unsigned dsize, cp;
    if (!d88_open()) return 0;
    if (t < 0 || t >= 164) return 0;
    base = d88_base(img);
    if (base < 0) return 0;
    cyl = t >> 1; head = t & 1;
    block = d88_u32(base + 0x20 + (long)(cyl * 2 + head) * 4);
    if (block == 0 || base + (long)block + 16 > d88_size) return 0;
    pos = base + (long)block;
    count = (int)d88_u16(pos + 4);   /* sectors in this track */
    if (count <= 0 || count > 64) count = 64;
    for (n = 0; n < count && pos + 16 <= d88_size; n++) {
        dsize = d88_u16(pos + 0x0E);
        if (dsize == 0) dsize = 128u << (d88[pos + 3] & 7);
        if (d88[pos + 2] == r) {
            cp = dsize < 256 ? dsize : 256;
            if (pos + 16 + (long)cp > d88_size) return 0;
            memcpy(d88 + pos + 16, src, cp);
            return 1;
        }
        pos += 16 + (long)dsize;
    }
    return 0;
}

/* public raw sector access (track = cyl*2+head, r = sector id) for
 * the Disk BASIC file layer (p88disk.c), which addresses the boot
 * disk (image 0) only */
int vessel_d88_sector(int t, int r, unsigned char *dst)
{
    return d88_read_sector_img(0, t, r, dst);
}

/* ================= disk subsystem command processor ================= */

/* output queue holds a whole read buffer plus a few status bytes */
#define OQ_SIZE 0x4400u
static unsigned char outq[OQ_SIZE];
static unsigned oq_head = 0, oq_count = 0;

static unsigned char read_buf[16384];
static unsigned read_len = 0;
static int read_valid = 0;
static unsigned char write_buf[16384];

static int sub_cmd = -1;        /* current command, -1 = idle */
static int sub_need = 0, sub_npar = 0;
static int sub_par[4];
static long sub_wleft = 0;      /* write data bytes still expected */
static int sub_err = 0;
static int surface_mode = 0;

static void oq_push(int v)
{
    if (oq_count < OQ_SIZE) { outq[(oq_head + oq_count) % OQ_SIZE] = (unsigned char)v; oq_count++; }
}

static int oq_pop(void)
{
    int v;
    if (!oq_count) return 0xFF;
    v = outq[oq_head];
    oq_head = (oq_head + 1) % OQ_SIZE; oq_count--;
    return v;
}

/* Read / write commands (02H / 01H): parameters are sector count,
 * drive, track, sector. The drive number selects the disk image
 * bundled in the D88 file - drive n holds image n, the mapping the
 * XDE services use for their disk argument - so a drive with no
 * image fails like any unreadable sector (d88_base). A request for
 * more sectors than the transfer buffer holds (64) is refused whole
 * in both directions: nothing is read or written, and the error
 * reaches the guest through the status command (06H, bit 0).
 * read_len is published only once every sector of the request has
 * been read: a read that fails part-way leaves it 0, so the send-data
 * command (03H / 12H) hands the guest nothing, not the sectors that
 * happened to precede the bad one. */
static void subsys_read(void)
{
    int nsec = sub_par[0], drv = sub_par[1], t = sub_par[2], r = sub_par[3], k;
    read_len = 0; read_valid = 0;
    if (nsec > (int)(sizeof(read_buf) / 256)) { sub_err = 1; return; }
    for (k = 0; k < nsec; k++) {
        if (!d88_read_sector_img(drv, t, r, read_buf + (unsigned)k * 256)) { sub_err = 1; return; }
        if (++r > 16) { r = 1; t++; }
    }
    read_len = (unsigned)nsec * 256;
    read_valid = 1; sub_err = 0;
}

static void subsys_write(void)
{
    int nsec = sub_par[0], drv = sub_par[1], t = sub_par[2], r = sub_par[3], k;
    sub_err = 0;
    if (nsec > (int)(sizeof(write_buf) / 256)) { sub_err = 1; return; }
    for (k = 0; k < nsec; k++) {
        if (!d88_write_sector_img(drv, t, r, write_buf + (unsigned)k * 256)) { sub_err = 1; return; }
        if (++r > 16) { r = 1; t++; }
    }
}

static void subsys_complete(void)
{
    unsigned k;
    switch (sub_cmd) {
    case 0x00: sub_err = 0; break;
    case 0x01: subsys_write(); break;
    case 0x02: subsys_read(); break;
    case 0x03: case 0x12:
        for (k = 0; k < read_len; k++) oq_push(read_buf[k]);
        break;
    case 0x06:
        oq_push(0x80 | (read_valid ? 0x40 : 0) | (sub_err ? 1 : 0));
        sub_err = 0;
        break;
    case 0x07: oq_push(0x80); break;
    case 0x17: surface_mode = sub_par[0]; break;
    case 0x18: oq_push(surface_mode); break;
    case 0x23: oq_push(0x00); break;
    }
    if (tracing()) {
        int i;
        fprintf(stderr, "SUBSYS cmd %02X", sub_cmd);
        for (i = 0; i < sub_need; i++) fprintf(stderr, " %02X", sub_par[i]);
        if (sub_cmd == 0x02) fprintf(stderr, " -> %s len %u", read_valid ? "ok" : "error", read_len);
        if (sub_cmd == 0x01) fprintf(stderr, " -> %s", sub_err ? "error" : "ok");
        fprintf(stderr, " (out queue %u)\n", oq_count);
    }
    sub_cmd = -1;
}

static void subsys_command(int b)
{
    sub_cmd = b; sub_npar = 0; sub_wleft = 0;
    switch (b) {
    case 0x01: case 0x02: sub_need = 4; break;
    case 0x17: case 0x23: sub_need = 1; break;
    case 0x00: case 0x03: case 0x06: case 0x07: case 0x12: case 0x18: sub_need = 0; break;
    default:
        if (tracing()) fprintf(stderr, "SUBSYS unknown cmd %02X\n", b);
        sub_need = 0; sub_err = 1; sub_cmd = -1;
        return;
    }
    if (sub_need == 0) subsys_complete();
}

static void subsys_data(int b)
{
    if (sub_cmd < 0) {
        if (tracing()) fprintf(stderr, "SUBSYS stray data %02X\n", b);
        return;
    }
    if (sub_npar < sub_need) {
        sub_par[sub_npar++] = b;
        if (sub_npar == sub_need) {
            if (sub_cmd == 0x01) { sub_wleft = (long)sub_par[0] * 256; if (sub_wleft) return; }
            subsys_complete();
        }
        return;
    }
    if (sub_wleft > 0) {
        long idx = (long)sub_par[0] * 256 - sub_wleft;
        if (idx >= 0 && idx < (long)sizeof(write_buf))
            write_buf[idx] = (unsigned char)b;
        if (--sub_wleft == 0) subsys_complete();
    }
}

/* ================= I/O ports ================= */

/* 8255 port C bits driven by the host (written via FEh or FFh bit set/reset) */
#define C_ATN 0x80
#define C_DAC 0x40
#define C_RFD 0x20
#define C_DAV 0x10

static int cfg_a_input = 1, cfg_b_input = 0;
static int ppi_mode = 0;
static int hostc = 0;           /* host control bits (bits 4-7) */
static int prev_c = 0;          /* hostc as of the previous pump */
static int latch = 0xFF;        /* last byte written by the host */
static int latched = 0;
static int sub_dac = 0;         /* subsystem DAC status */
static unsigned char misc_port[256];

/* Graphics palette registers (ports 0x54-0x5B), 3 bits per component.
 * One register set shared by direct OUT writes from machine code and
 * by the interpreter's PALETTE statement (via hal_palette's mirror in
 * halp8.c), as on hardware. Reset state is the digital palette. */
static unsigned char vram_pal[8][3];    /* [i] = {r, g, b}, 0..7 */

void vessel_palette(int idx, int *r, int *g, int *b)
{
    if (idx < 0) idx = 0;
    if (idx > 7) idx = 7;
    *r = vram_pal[idx][0] * 255 / 7;
    *g = vram_pal[idx][1] * 255 / 7;
    *b = vram_pal[idx][2] * 255 / 7;
}

/* Port 0x53 overlay state (docs/APPROXIMATIONS.md item 31).
 * The byte is written on the engine thread (OUT, the SCREEN reset,
 * the direct-mode restore) and read by the host's rendering thread
 * through the two accessors below, so every access goes through
 * these relaxed atomic helpers. A single byte with no ordering
 * requirement against other state: relaxed is enough. */
static void pile_store(unsigned char v)
{
    __atomic_store_n(&misc_port[0x53], v, __ATOMIC_RELAXED);
}
static unsigned char pile_load(void)
{
    return __atomic_load_n(&misc_port[0x53], __ATOMIC_RELAXED);
}

int vessel_text_hidden(void) { return (pile_load() & 0x01) != 0; }

/* Hidden graphics planes as a mask over the frame's colour index
 * (B = bit 0, R = bit 1, G = bit 2, as composed in engine.c
 * p8_frame). The port's bits 0x02/0x04/0x08 are B/R/G in that same
 * order, one position higher. */
int vessel_pile_plane_mask(void) { return (pile_load() >> 1) & 0x07; }

/* The ROM's SCREEN statement puts the overlay back to "show all"
 * (called through hal_pile_reset from stmt_screen, and from
 * top_level when the interpreter goes back to direct mode). */
void vessel_pile_reset(void) { pile_store(0); }

void vessel_set_palette(int idx, int r7, int g7, int b7)
{
    if (idx < 0 || idx > 7) return;
    vram_pal[idx][0] = (unsigned char)r7;
    vram_pal[idx][1] = (unsigned char)g7;
    vram_pal[idx][2] = (unsigned char)b7;
}

static void palette_reset(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        vram_pal[i][0] = (i & 2) ? 7 : 0;
        vram_pal[i][1] = (i & 4) ? 7 : 0;
        vram_pal[i][2] = (i & 1) ? 7 : 0;
    }
}

/* RAM window at 8000-83FF (I/O 70H) and 64K-RAM mode (I/O 31H).
 * The window normally shows the 1K page of main RAM whose base
 * OUT 70H sets (base = value << 8; IN 70H reads it back). With
 * port 31H bit 1 (64K-RAM) or bit 2 (N) set, the window is the
 * identity: the CPU sees the RAM at its own address (QUASI88's
 * main_memory_mapping_8000_83ff). The base starts at 8000H -
 * the identity a booted N88-BASIC presents - so nothing changes
 * unless the guest reprograms the window or the mode. Only Z80
 * bus accesses are translated; the vessel's own services keep
 * addressing flat RAM, as a DMA transfer would. */
static unsigned window_base = 0x8000;

static uint16_t win_addr(uint16_t addr)
{
    if (misc_port[0x31] & 0x06) return addr;
    return (uint16_t)(window_base + (addr & 0x3FF));
}

/* Graphics VRAM and the C000-FFFF window (PC-88 memory banking).
 * On the real machine the CPU window C000-FFFF can be switched away
 * from main RAM to the graphics planes:
 *   - ports 5Ch/5Dh/5Eh select plane B/R/G (bank mode), 5Fh = main RAM;
 *   - port 32h bit 6 (EVRAM) + port 35h bit 7 (VACCESS) select the ALU
 *     extended-access mode, where port 34h (ALU1) picks per-plane
 *     set/clear/xor/hold and port 35h bits 4-5 the ALU mode.
 * NEOMSC's display services draw through this window (glyph copies via
 * the ALU, CG decompression via the banks). Without it the draws land
 * in flat RAM and destroy the song table at C800 and song data at
 * CA00/5500-adjacent ranges -- the root cause of the post-load-base
 * MENU failure (line-2040 overflow, silent attract). Only the Z80 bus
 * view is banked; the interpreter's own PEEK/POKE stay on flat RAM,
 * matching how the drivers always restore MAIN before returning. */
/* Kanji ROM port interface (ports E8-EF): the guest latches a 16-bit
 * glyph address (E8=low, E9=high for ROM 1; EC/ED for ROM 2) and reads
 * the glyph row back a byte at a time - IN E9 returns byte [0] (the
 * left half of the row), IN E8 byte [1] (the right half). The glyph
 * data itself is a substitute font generated from an open-licensed
 * source (src/kanjifont.c); no NEC ROM dump is used. EA/EB writes are
 * accepted and ignored, matching QUASI88's behaviour. */
extern const unsigned char p8_kanji_glyph[2][4096][32];
static unsigned short kanji_addr[2];

static unsigned char gvram[3][0x4000];  /* planes: 0=B, 1=R, 2=G */
static int mem_bank = 3;                /* 0..2 = plane, 3 = main RAM */
static int intr_level = 7;              /* port E4 priority level: the
                                         * sound interrupt needs >= 5,
                                         * VSYNC (VRTC) >= 2 (QUASI88's
                                         * intr model; PMD's mstop drops
                                         * the level to 2 to silence the
                                         * sequencer, mstart raises 5) */
static int intr_mask = 0xFF;            /* port E6 interrupt mask, 1 =
                                         * enabled: bit 0 RTC, bit 1 VSYNC,
                                         * bit 2 SIO (QUASI88 pc88main.cpp
                                         * case 0xe6). Power-on here is
                                         * "nothing masked", the state a
                                         * booted BASIC leaves. The sound
                                         * interrupt is NOT in this port:
                                         * its mask is port 0x32 bit 7
                                         * (SINTM, 1 = masked). */
static void vrtc_retract(void);         /* defined with the interrupt model
                                         * below; the E6 write calls it */
static unsigned long alu_buf = 0;       /* ALU read latch (32-bit) */

static int vram_way(void)               /* 0 = RAM, 1 = bank, 2 = ALU */
{
    if ((misc_port[0x32] & 0x40) && (misc_port[0x35] & 0x80)) return 2;
    if (mem_bank != 3) return 1;
    return 0;
}

static unsigned char alu_read(unsigned addr)
{
    /* ALU2 bits 0-2 are the compare data: the read returns the AND,
     * over the three planes, of (plane byte) where the plane's
     * compare bit is 1 and of (~plane byte) where it is 0. The
     * 3-plane word is latched for the ALU write-back modes. */
    unsigned char r = 0xFF;
    int i;
    alu_buf = (unsigned long)gvram[0][addr]
            | ((unsigned long)gvram[1][addr] << 8)
            | ((unsigned long)gvram[2][addr] << 16);
    for (i = 0; i < 3; i++) {
        unsigned char b = gvram[i][addr];
        r &= (misc_port[0x35] & (1 << i)) ? b : (unsigned char)~b;
    }
    return r;
}

static void alu_write(unsigned addr, unsigned char data)
{
    int i, mode;
    switch (misc_port[0x35] & 0x30) {
    case 0x00:                          /* per-plane set/clear/xor/hold */
        mode = misc_port[0x34];
        for (i = 0; i < 3; i++, mode >>= 1) {
            switch (mode & 0x11) {
            case 0x00: gvram[i][addr] &= (unsigned char)~data; break;
            case 0x01: gvram[i][addr] |= data; break;
            case 0x10: gvram[i][addr] ^= data; break;
            default: break;             /* 0x11: hold */
            }
        }
        break;
    case 0x10:                          /* latched word write-back */
        gvram[0][addr] = (unsigned char)(alu_buf & 0xFF);
        gvram[1][addr] = (unsigned char)((alu_buf >> 8) & 0xFF);
        gvram[2][addr] = (unsigned char)((alu_buf >> 16) & 0xFF);
        break;
    case 0x20:
        gvram[0][addr] = (unsigned char)((alu_buf >> 8) & 0xFF);
        break;
    default:
        gvram[1][addr] = (unsigned char)(alu_buf & 0xFF);
        break;
    }
}

/* Read-only access for the engine's frame composer. */
const unsigned char *vessel_vram_plane(int bank)
{
    if (bank < 0 || bank > 2) return 0;
    return gvram[bank];
}

/* Write-through for the interpreter's graphics statements: on the
 * genuine machine BASIC and the guest draw into the same GVRAM, so
 * a BASIC erase must also clear what the guest plotted. x is
 * 0..639, y is in 400-line frame coordinates (two frame lines per
 * VRAM line). Color bits that are 0 clear the plane bit. */
void vessel_vram_pset(int x, int y, int c)
{
    unsigned addr;
    unsigned char bit;
    int p;
    if (x < 0 || x >= 640 || y < 0 || y >= 400) return;
    addr = (unsigned)((y >> 1) * 80 + (x >> 3));
    bit = (unsigned char)(0x80 >> (x & 7));
    for (p = 0; p < 3; p++) {
        if ((c >> p) & 1) gvram[p][addr] |= bit;
        else gvram[p][addr] &= (unsigned char)~bit;
    }
}

/* Whole-screen fill with color c: only the visible 80x200 bytes of
 * each plane (the tail up to 0x4000 is guest-addressable memory a
 * BASIC clear must not touch). */
void vessel_vram_clear(int c)
{
    int p;
    for (p = 0; p < 3; p++)
        memset(gvram[p], ((c >> p) & 1) ? 0xFF : 0x00, 80 * 200);
}

/* Pixel read in the same frame coordinates as vessel_vram_pset:
 * the 3-plane color index 0..7, 0 when out of range. */
int vessel_vram_gpoint(int x, int y)
{
    unsigned addr;
    int p, sh, v = 0;
    if (x < 0 || x >= 640 || y < 0 || y >= 400) return 0;
    addr = (unsigned)((y >> 1) * 80 + (x >> 3));
    sh = 7 - (x & 7);
    for (p = 0; p < 3; p++) v |= ((gvram[p][addr] >> sh) & 1) << p;
    return v;
}

/* OPNA (YM2608) register file. Both PC-88 port pairs are tracked:
 * 44h/45h + 46h/47h and A8h-A9h + AAh/ABh (bank 0 / bank 1).
 * Bank 0 registers 26h (Timer B counter) and 27h (timer control)
 * drive the interrupt model below. */
static int opn_addr[2];
static unsigned char opn_reg[2][256];
static unsigned long opn_nwrite[256];   /* bank-0 write counts */
static unsigned long opn_nwrite1[256];  /* bank-1 write counts */
static int tb_flag = 0;                 /* Timer B status flag */
static int tb_line = 0;                 /* current level of the sound
                                         * interrupt line into the
                                         * controller (tb_line_update) */
static int tb_due = 0;                  /* controller's latched sound request */

/* Sound interrupt line as the controller sees it: the chip's Timer B
 * request (flag set, IRQ enable B = reg 0x27 bit 3) masked by port
 * 0x32 bit 7 (SINTM) BEFORE the controller, which latches the rising
 * edge (QUASI88's model). An overflow under SINTM reaches the
 * controller only if the flag is still standing when the guest
 * unmasks, and an edge already latched survives a later SINTM.
 *
 * Must be called at every point where one of the line's inputs
 * changes (overflow, reg 0x27 write, port 0x32 write), not only once
 * per irq_advance: an acknowledge and the next overflow can both fall
 * between two irq_advance samplings, and the drop in between is what
 * makes that overflow a new edge. All paths share tb_line, so a rise
 * latched at a write is not latched again by the next sampling. */
static void tb_line_update(void)
{
    int line = tb_flag && !(misc_port[0x32] & 0x80);
    if (line && !tb_line) tb_due = 1;
    tb_line = line;
}

static void tb_restart(void);           /* with the interrupt model below */

/* Bank-0 data-port read (QUASI88 sound_in_data): registers 00H-0FH
 * (the SSG block) read back what was written, FFH is the board-ID
 * register - `id` is this port's answer, 1 = Sound Board II - and
 * every other register is write-only and reads 0. */
static int opn_data_read(int id)
{
    int a = opn_addr[0] & 0xFF;
    if (a < 0x10) return opn_reg[0][a];
    if (a == 0xFF) return id;
    return 0;
}

static void opn_write(int bank, int addr, int v)
{
    int load_b = bank == 0 && addr == 0x27 && (opn_reg[0][0x27] & 0x02);
    opn_reg[bank][addr] = (unsigned char)v;
    opna_write(bank, addr, v);          /* sound rendering sink (ymfm) */
    if (bank == 1) { opn_nwrite1[addr]++; return; }
    if (bank == 0) {
        opn_nwrite[addr]++;
        if (addr == 0x27) {
            /* LOAD B (bit 1) rising: the counter loads the period.
             * A timer stopped and started again begins a whole
             * period, it does not resume the one it was stopped in
             * (QUASI88 check_sound_parm_update; ymfm update_timer). */
            if ((v & 0x02) && !load_b) tb_restart();
            if (v & 0x20) tb_flag = 0;                  /* RST B (bit 5) */
            tb_line_update();   /* flag reset / IRQ enable B change */
        }
        if (tracing() && (addr == 0x26 || addr == 0x27))
            fprintf(stderr, "VESSEL OPN reg %02X <- %02X\n", addr, v);
    }
}

/* advance the handshake on DAV / DAC edges since the previous call */
static void subsys_pump(void)
{
    int rise = hostc & ~prev_c, fall = ~hostc & prev_c;
    if (rise & C_DAV) {
        if (latched) {
            if (hostc & C_ATN) subsys_command(latch); else subsys_data(latch);
            latched = 0;
        }
        sub_dac = 1;
    }
    if (fall & C_DAV) sub_dac = 0;
    if ((rise & C_DAC) && oq_count) oq_pop();
    prev_c = hostc;
}

static int ppi_status(void)
{
    int dav = oq_count > 0 && !(hostc & C_DAC);
    int rfd = 1;    /* bytes are processed synchronously: input never full */
    return (hostc & 0xF0) | (sub_dac << 2) | (rfd << 1) | dav;
}

static void note_port(unsigned char *map, const char *dir, int port, int v)
{
    if (tracing() && first_time(map, (unsigned)port)) {
        if (v >= 0) fprintf(stderr, "VESSEL %s %02X <- %02X\n", dir, port, v);
        else fprintf(stderr, "VESSEL %s %02X\n", dir, port);
    }
}

int vessel_port_in(int port)
{
    if (port >= 0xFC) {
        subsys_pump();
        switch (port) {
        case 0xFC: return cfg_a_input ? (oq_count ? outq[oq_head] : 0xFF) : latch;
        case 0xFD: return latch;
        case 0xFE: return ppi_status();
        default: return 0xFF;
        }
    }
    note_port(in_seen, "IN", port, -1);
    if (port <= 0x0E) return hal_keymatrix(port);   /* keyboard matrix rows */
    switch (port) {
    /* OPNA read ports. One chip answers on both decodes (44H-47H and
     * A8H-ADH), port for port as in QUASI88 (pc88main.cpp main_io_in,
     * soundbd.cpp):
     *   44H / A8H  status: bit 1 = Timer B flag, never busy. Taken
     *              from the vessel's own timer model, not from ymfm's
     *              read_status(): the wrapper has no ymfm_set_timer
     *              backend, so the chip's own timer flags never set.
     *              (Timer A is not modelled: bit 0 reads 0.)
     *   45H / A9H  bank-0 data port (opn_data_read)
     *   46H / ACH  extended status from ymfm's read_status_hi():
     *              ADPCM-B EOS (bit 2) / BRDY (bit 3) / PLAYING
     *              (bit 5) under the guest's flag mask (bank-1
     *              register 10H). NEOMSC's ADPCM transfer polls BRDY
     *              on 46H, PMD's eoscheck polls EOS on ACH. Bits 0-1
     *              (the timer flags) read 0 here for the reason above.
     *   47H / ADH  bank-1 data port: the ADPCM-B memory read
     *              (register 08H), 0 for the other registers. */
    case 0x44: case 0xA8: return tb_flag ? 0x02 : 0x00;
    /* The board-ID register (FFH) answers 1 = Sound Board II on both
     * decodes, as in QUASI88: MENU.ALP probes each one (190:
     * OUT &H44,255:A=INP(&H45):IF A=1, 200: the A8H/A9H twin). */
    case 0x45: case 0xA9: return opn_data_read(1);
    case 0x46: case 0xAC: return opna_status_hi();
    case 0x47: case 0xAD: return opna_data_hi(opn_addr[1] & 0xFF);
    case 0x31: case 0x32: case 0x34: case 0x35: return misc_port[port];
    case 0x70: return (int)(window_base >> 8);   /* RAM window base */
    case 0xE8: return p8_kanji_glyph[0][kanji_addr[0] >> 4][(kanji_addr[0] & 15) * 2 + 1];
    case 0xE9: return p8_kanji_glyph[0][kanji_addr[0] >> 4][(kanji_addr[0] & 15) * 2 + 0];
    case 0xEC: return p8_kanji_glyph[1][kanji_addr[1] >> 4][(kanji_addr[1] & 15) * 2 + 1];
    case 0xED: return p8_kanji_glyph[1][kanji_addr[1] >> 4][(kanji_addr[1] & 15) * 2 + 0];
    }
    return 0xFF;
}

void vessel_port_out(int port, int v)
{
    if (port >= 0xFC) {
        subsys_pump();
        switch (port) {
        case 0xFC: if (!cfg_a_input) { latch = v; latched = 1; } break;
        case 0xFD: latch = v; latched = 1; break;
        case 0xFE: hostc = v & 0xF0; break;
        case 0xFF:
            if (v & 0x80) {
                ppi_mode = v;
                cfg_a_input = (v >> 4) & 1;
                cfg_b_input = (v >> 1) & 1;
            } else {
                int bit = (v >> 1) & 7;
                if (bit >= 4) {
                    if (v & 1) hostc |= 1 << bit; else hostc &= ~(1 << bit);
                }
            }
            break;
        }
        subsys_pump();
        return;
    }
    note_port(out_seen, "OUT", port, v);
    switch (port) {
    case 0x44: case 0xA8: opn_addr[0] = v; break;
    case 0x45: case 0xA9: opn_write(0, opn_addr[0], v); break;
    /* NOTE: the 0xAA/0xAB bank-1 alias predates the 0xAC/0xAD fix.
     * On genuine hardware (QUASI88 pc88main.cpp) OUT 0xAA is the
     * sound interrupt-mask write, so this alias can steal a mask
     * value into the bank-1 address latch; no guest in the current
     * corpus is known to need the alias. Flagged in
     * docs/APPROXIMATIONS.md item 20 for the multi-title check. */
    case 0x46: case 0xAA: opn_addr[1] = v; break;
    case 0x47: case 0xAB: opn_write(1, opn_addr[1], v); break;
    /* PMD88 addresses the OPNA's bank 1 through the 0xAC/0xAD pair
     * (the board's A8-AF decode; QUASI88 pc88main.cpp cases 0xac/0xad).
     * Without these, every ADPCM-B register write from PMD was dropped
     * and the PCM part played silently. */
    case 0xAC: opn_addr[1] = v; break;
    case 0xAD: opn_write(1, opn_addr[1], v); break;
    case 0x31: case 0x34: case 0x35: misc_port[port] = (unsigned char)v; break;
    /* Screen overlay ("pile") register (QUASI88 pc88main.cpp case
     * 0x53: grph_pile). bit0 hides TEXT, bits 1/2/3 hide the B/R/G
     * graphics planes; consulted by the frame/text compositor through
     * vessel_text_hidden / vessel_pile_plane_mask (engine.c).
     * Atomic store: the rendering thread reads this byte. */
    case 0x53: pile_store((unsigned char)v); break;
    case 0x32:
        misc_port[port] = (unsigned char)v;
        tb_line_update();       /* SINTM (bit 7) gates the sound line */
        break;
    case 0x70: window_base = (unsigned)(v & 0xFF) << 8; break;
    case 0xE4: intr_level = (v & 0x08) ? 7 : (v & 0x07); break;
    /* Interrupt mask. Masking VSYNC also drops a request that is
     * already latched (QUASI88: VSYNC_flag = false) - including one
     * already queued with the CPU core but not yet accepted - and
     * none is latched while it stays masked (irq_advance). */
    case 0xE6:
        intr_mask = v & 0xFF;
        if (!(intr_mask & 0x02)) vrtc_retract();
        break;
    case 0xE8: kanji_addr[0] = (unsigned short)((kanji_addr[0] & 0xFF00) | (v & 0xFF)); break;
    case 0xE9: kanji_addr[0] = (unsigned short)((kanji_addr[0] & 0x00FF) | ((v & 0xFF) << 8)); break;
    case 0xEC: kanji_addr[1] = (unsigned short)((kanji_addr[1] & 0xFF00) | (v & 0xFF)); break;
    case 0xED: kanji_addr[1] = (unsigned short)((kanji_addr[1] & 0x00FF) | ((v & 0xFF) << 8)); break;
    case 0xEA: case 0xEB: break;   /* accepted, ignored (QUASI88 parity) */
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: {
        /* Palette registers (QUASI88 pc88main.cpp): digital mode packs
         * one bit per component; analog mode takes two writes - with
         * bit 6 clear, blue = bits 0-2 and red = bits 3-5; with bit 6
         * set, green = bits 0-2. The other components are kept. */
        int i = port - 0x54;
        if (!(misc_port[0x32] & 0x20)) {
            vram_pal[i][2] = (v & 1) ? 7 : 0;
            vram_pal[i][0] = (v & 2) ? 7 : 0;
            vram_pal[i][1] = (v & 4) ? 7 : 0;
        } else if (!(v & 0x40)) {
            vram_pal[i][2] = v & 7;
            vram_pal[i][0] = (v >> 3) & 7;
        } else {
            vram_pal[i][1] = v & 7;
        }
        break; }
    case 0x5C: mem_bank = 0; break;     /* VRAM plane B */
    case 0x5D: mem_bank = 1; break;     /* VRAM plane R */
    case 0x5E: mem_bank = 2; break;     /* VRAM plane G */
    case 0x5F: mem_bank = 3; break;     /* main RAM */
    }
    if (port >= 0x5C && port <= 0x5F && getenv("P8P_TRACE_BANK")) {
        static int nb = 0;
        if (nb < 200) { nb++; fprintf(stderr, "BANK %02X pc=%04X\n", port, (unsigned)cpu.pc); }
    }
}

/* ================= CPU ================= */

static uint8_t mem_read(void *ud, uint16_t addr)
{
    (void)ud;
    if (addr >= 0xC000) {
        int w = vram_way();
        if (w == 2) return alu_read(addr & 0x3FFF);
        if (w == 1) return gvram[mem_bank][addr & 0x3FFF];
    }
    if (addr >= 0x8000 && addr < 0x8400) addr = win_addr(addr);
    return (uint8_t)hal_peek(0, addr);
}

static void mem_write(void *ud, uint16_t addr, uint8_t v)
{
    (void)ud;
    /* P8P_WATCH=lo-hi (hex): log Z80 writes in range with the
     * writer's pc, capped, for display-path investigation. */
    static int watch_lo = -1, watch_hi = -1, watch_left = 4000;
    if (watch_lo < 0) {
        const char *w = getenv("P8P_WATCH");
        watch_lo = 0; watch_hi = -1;
        if (w) {
            unsigned lo, hi;
            if (sscanf(w, "%x-%x", &lo, &hi) == 2) {
                watch_lo = (int)lo; watch_hi = (int)hi;
            }
        }
    }
    if (watch_hi >= 0 && (int)addr >= watch_lo && (int)addr <= watch_hi
        && watch_left > 0) {
        watch_left--;
        fprintf(stderr, "ZW %04X <- %02X pc=%04X\n", addr, v,
                (unsigned)cpu.pc);
    }
    if (addr >= 0xC000) {
        int w = vram_way();
        if (w == 2) { alu_write(addr & 0x3FFF, v); return; }
        if (w == 1) { gvram[mem_bank][addr & 0x3FFF] = v; return; }
    }
    if (addr >= 0x8000 && addr < 0x8400) addr = win_addr(addr);
    hal_poke(0, addr, v);
}

static uint8_t io_in(z80 *z, uint8_t port)
{
    (void)z;
    return (uint8_t)vessel_port_in(port);
}

static void io_out(z80 *z, uint8_t port, uint8_t v)
{
    (void)z;
    vessel_port_out(port, v);
}

static void emu_ret(void)
{
    cpu.pc = (uint16_t)rd16(cpu.sp);
    cpu.sp = (uint16_t)(cpu.sp + 2);
}

/* Read a numeric literal from the token stream at HL (the forms the
 * PC-88 tokenizer produces): digit tokens / ASCII digits, 0x0F int8,
 * 0x1C int16, 0x0C / 0x0B u16. Returns the value; *php advances. */
static long stream_number(unsigned *php)
{
    unsigned p = *php;
    long v = 0;
    int neg = 0, any = 0;
    while (hal_peek(0, p) == 0x20) p++;
    if (hal_peek(0, p) == 0x2D) { neg = 1; p++; }
    for (;;) {
        int b = hal_peek(0, p);
        if (b >= 0x11 && b <= 0x1A) { v = v * 10 + (b - 0x11); p++; any = 1; }
        else if (b >= '0' && b <= '9') { v = v * 10 + (b - '0'); p++; any = 1; }
        else break;
    }
    if (!any) {
        int b = hal_peek(0, p);
        if (b == 0x0F) { v = (signed char)hal_peek(0, p + 1); p += 2; }
        else if (b == 0x1C) { v = (short)(hal_peek(0, p + 1) | (hal_peek(0, p + 2) << 8)); p += 3; }
        else if (b == 0x0C || b == 0x0B) { v = hal_peek(0, p + 1) | (hal_peek(0, p + 2) << 8); p += 3; }
    }
    *php = p & 0xFFFF;
    return neg ? -v : v;
}

/* Prime the argument descriptor with the FIRST argument's value, the
 * way the genuine interpreter's extension-statement dispatcher does
 * before it jumps through the EEA1/EEA4 vectors: the FFDD/FFDE handler
 * heads fetch their service number by calling the ROM evaluator helper
 * (0x9F84), which returns (0xEC41) — a cell the dispatcher has already
 * filled. Our vessel cannot run the ROM evaluator, so the relay primes
 * the descriptor itself; without this the head reads a stale value,
 * the CP 8 / CP 9 dispatch misses, and the call falls into the generic
 * parameter collector (found 2026-11-18 via the PROBE39 step trace:
 * XDD 8 never reached the CG decompressor). */
void vessel_prime_arg(long v)
{
    hal_poke(0, 0xEC44, 2);
    hal_poke(0, 0xEC45, (int)(v & 0xFF));
    hal_poke(0, 0xEC46, (int)((v >> 8) & 0xFF));
    hal_poke(0, 0xEC41, (int)(v & 0xFF));
    hal_poke(0, 0xEC42, (int)((v >> 8) & 0xFF));
}

/* Skip one token at p in a BASIC token stream (the forms the PC-88
 * tokenizer / our synthesizer produce): a quoted string, a run of
 * digit tokens / ASCII digits (one number), a prefixed numeric
 * constant, or a single command letter. Returns the new pointer. */
static unsigned stream_skip_token(unsigned p)
{
    int b = hal_peek(0, p);
    if (b == 0x22) {                        /* quoted string */
        p++;
        while (hal_peek(0, p) != 0x22 && hal_peek(0, p) != 0) p++;
        if (hal_peek(0, p) == 0x22) p++;
        return p;
    }
    if ((b >= 0x11 && b <= 0x1A) || (b >= '0' && b <= '9')) {
        while ((hal_peek(0, p) >= 0x11 && hal_peek(0, p) <= 0x1A)
               || (hal_peek(0, p) >= '0' && hal_peek(0, p) <= '9')) p++;
        return p;
    }
    if (b == 0x0F) return p + 2;            /* int8 */
    if (b == 0x1C || b == 0x0C || b == 0x0B || b == 0x0E) return p + 3;
    if (b == 0x1D) return p + 5;            /* MBF single */
    if (b == 0x1F) return p + 9;            /* MBF double */
    return p + 1;                           /* letter / anything else */
}

/* check PC traps: -1 = stop, 1 = handled (PC changed), 0 = execute normally */
static int trap(void)
{
    unsigned pc = cpu.pc, ret;
    if (pc == SENTINEL) return -1;
    if (pc == 0x0010) {     /* RST 0x10: ROM CHRGET -- A = (HL), HL
                             * advanced past it and any spaces. NEOMSC's
                             * FFDD handler calls it on the ';' of a data
                             * reference (XDD fn,..,x,y;song,type) to step
                             * HL onto the song expression before fetching
                             * it with the usual evaluator calls. */
        unsigned hl = ((unsigned)cpu.h << 8) | cpu.l;
        unsigned char ch;
        do { ch = hal_peek(0, hl++); } while (ch == ' ');
        cpu.a = ch;
        cpu.h = (unsigned char)(hl >> 8);
        cpu.l = (unsigned char)hl;
        cpu.zf = (ch == 0 || ch == ':');
        cpu.cf = (ch >= '0' && ch <= '9');
        emu_ret();
        return 1;
    }
    if (pc == 0x0008) {     /* RST 8: inline service byte follows the RST */
        ret = rd16(cpu.sp);
        if (tracing()) { fprintf(stderr, "VESSEL RST8 svc %02X at %04X", hal_peek(0, ret), ret); trace_regs(); }
        if (hal_peek(0, ret) == 0x2C) {
            /* svc 0x2C: advance HL past the argument the caller just
             * consumed (HL points AT that token on entry — the ROM
             * evaluator advances its own pointer, not HL, so this
             * service is what re-syncs HL), skip the separator,
             * evaluate the next argument and leave its descriptor at
             * (0xEC41): [type, lo, hi]. The driver's readers fetch it
             * from there. If HL already sits on the separator (the
             * FFDF handler consumes its command letter itself), no
             * token is skipped. (Semantics pinned 2026-11-18 from the
             * PROBE39 step trace: with the old skip-separator-only
             * reading, every collector store landed one argument
             * early and the FFDD CG path re-read its service number
             * as the CG address.) */
            unsigned p = (unsigned)cpu.h << 8 | cpu.l;
            long v;
            while (hal_peek(0, p) == 0x20) p++;
            if (hal_peek(0, p) != 0x2C) p = stream_skip_token(p);
            while (hal_peek(0, p) == 0x20) p++;
            if (hal_peek(0, p) == 0x2C) p++;
            while (hal_peek(0, p) == 0x20) p++;
            v = stream_number(&p);
            hal_poke(0, 0xEC44, 2);
            hal_poke(0, 0xEC45, (int)(v & 0xFF));
            hal_poke(0, 0xEC46, (int)((v >> 8) & 0xFF));
            /* NEOMSC's in-image argument readers also fetch the value
             * directly at (0xEC41) [lo] / (0xEC42) [hi] — the FFDD
             * parameter collector stores (0xEC41) verbatim, which is
             * how the descriptor's home was identified (2026-11-18). */
            hal_poke(0, 0xEC41, (int)(v & 0xFF));
            hal_poke(0, 0xEC42, (int)((v >> 8) & 0xFF));
            cpu.h = (uint8_t)(p >> 8); cpu.l = (uint8_t)p;
        }
        cpu.sp = (uint16_t)(cpu.sp + 2);
        cpu.pc = (uint16_t)((ret + 1) & 0xFFFF);
        return 1;
    }
    if (pc == 0x18A3) {     /* ROM: fetch the evaluated value's low byte -> A */
        cpu.a = (uint8_t)hal_peek(0, 0xEC45);
        cpu.e = (uint8_t)hal_peek(0, 0xEC45);
        cpu.d = (uint8_t)hal_peek(0, 0xEC46);
        emu_ret();
        return 1;
    }
    if (pc == 0x11D3) {     /* ROM: evaluate the argument at HL and
                             * leave the result in the (0xEC41) value
                             * cell. For a number that cell holds the
                             * value itself; for a string it holds a
                             * POINTER to the 3-byte descriptor
                             * [len][addr lo][addr hi] — the N88-BASIC
                             * convention NEOMSC's FFDE service-3 draw
                             * path relies on when it dereferences
                             * (0xEC41) after this call. The relay's
                             * synthetic stream carries strings as
                             * quoted literals, so the descriptor can
                             * point straight at the literal's bytes.
                             * (Without this, service 3 drew whatever
                             * the stale numeric descriptor pointed at:
                             * a fixed, text-independent pattern.) */
        unsigned p = (unsigned)cpu.h << 8 | cpu.l;
        int b;
        while (hal_peek(0, p) == 0x20) p++;
        b = hal_peek(0, p);
        if (b == 0x22) {
            unsigned start = ++p, len;
            while (hal_peek(0, p) != 0x22 && hal_peek(0, p) != 0) p++;
            len = p - start;
            if (len > 255) len = 255;
            hal_poke(0, 0xEC50, (int)len);
            hal_poke(0, 0xEC51, (int)(start & 0xFF));
            hal_poke(0, 0xEC52, (int)((start >> 8) & 0xFF));
            hal_poke(0, 0xEC41, 0x50);
            hal_poke(0, 0xEC42, 0xEC);
            if (hal_peek(0, p) == 0x22) p++;
            cpu.h = (uint8_t)(p >> 8); cpu.l = (uint8_t)p;
        } else if ((b >= 0x11 && b <= 0x1A) || (b >= '0' && b <= '9')
                   || b == 0x0F || b == 0x1C || b == 0x0C || b == 0x0B
                   || b == 0x2D) {
            long v = stream_number(&p);
            vessel_prime_arg(v);
            cpu.h = (uint8_t)(p >> 8); cpu.l = (uint8_t)p;
        }
        /* Any other byte (the ',' separator or the stream
         * terminator) means the RST 8 resync has already evaluated
         * this argument and the evaluator is being asked for the
         * current value: leave (0xEC41) and HL exactly as they are.
         * Clobbering the cell here breaks NEOMSC's sub-function
         * dispatch, whose fetch (0x9F84) calls this evaluator with
         * HL sitting on the separator. */
        emu_ret();
        return 1;
    }
    if (pc == 0xA794 && neomsc_loaded) {
                            /* NEOMSC's argument fetch (armed only with
                             * NEOMSC detected; otherwise whatever code
                             * lives here runs as written): the in-image
                             * routine here chains into ROM evaluators
                             * (0x11D3 / 0x1B9D) that the vessel cannot run,
                             * so the fetch is emulated from the RST 8
                             * svc 0x2C descriptor: DE = the evaluated
                             * 16-bit argument. The address is absolute in
                             * NEOMSC's code and does NOT move with the
                             * load base (0x9900 per its BSAVE header). */
        cpu.e = (uint8_t)hal_peek(0, 0xEC45);
        cpu.d = (uint8_t)hal_peek(0, 0xEC46);
        emu_ret();
        return 1;
    }
    if (pc == 0x37C9 || pc == 0x37CC) { subsys_command(cpu.a); emu_ret(); return 1; }  /* PUTCOM */
    if (pc == 0x37D2) { subsys_data(cpu.a); emu_ret(); return 1; }                      /* PUTDAT */
    if (pc == 0x3847) { cpu.a = (uint8_t)oq_pop(); emu_ret(); return 1; }               /* GETDAT */
    if (pc == 0x6F6B) {
        if (tracing()) { fprintf(stderr, "VESSEL ROM 6F6B (registration)"); trace_regs(); }
        emu_ret();
        return 1;
    }
    if (pc < 0x8000) {
        if (tracing() && first_time(rom_seen, pc)) { fprintf(stderr, "VESSEL ROM %04X skipped from %04X", pc, rd16(cpu.sp)); trace_regs(); }
        emu_ret();
        return 1;
    }
    /* NEOMSC calls PMD's entries (0xAA00..) whether or not the program
     * has loaded PMD yet; until a load spanning 0xAA00 is announced
     * the range answers with a plain RET. Without NEOMSC detected the
     * range is ordinary memory and executes as written. */
    if (neomsc_loaded && pc >= PMD_START && pc <= PMD_END && !pmd_loaded) {
        if (tracing() && first_time(pmd_seen, pc - PMD_START)) fprintf(stderr, "VESSEL PMD %04X not loaded, skipped\n", pc);
        emu_ret();
        return 1;
    }
    return 0;
}

/* ================= interrupts (PMD's IM 2 pair) ================= */
/* PMD88 (KAJA) runs on two interrupts, installed by its init (rtlset)
 * into the Z80 IM 2 vector table at (I << 8): VRTC at +2 (fadeout and
 * effects, ~60 Hz; the handler chains to BASIC's VRTC handler at
 * 0xE808) and the OPNA interrupt at +8 (the sequencer main, clocked by
 * Timer B: counter register 0x26, control 0x27). On hardware the host
 * BASIC owns the I register and the table page; the vessel presets I
 * to a free page and delivers both interrupts through z80_gen_int.
 * Layout per the PMD88 v3.7 source (src2.txt L35-36, L111-223,
 * L225-267, L476-523); MA2026 bundles PMD v3.3, so the harness
 * verifies that the bundled binary registers the same slots. */
#define IRQ_PAGE    0xE9
#define VRTC_VEC    2
#define OPN_VEC     8
#define REST_CELL   0xE80A      /* planted RET: the at-rest mainline PC */
#define VRTC_PERIOD 66667L      /* 4 MHz CPU / 60 Hz */
#define PUMP_SLICE  16667L      /* emulated cycles per vessel_pump() */
/* Modelled cost of one BASIC statement, charged by vessel_statement()
 * on every pass of exec_loop: ~0.5 ms at 4 MHz, the middle of the
 * ROM interpreter's range for simple statements (roughly 500-4000
 * cycles for a PRINT / LOCATE / READ / POKE / NEXT mix). A documented
 * approximation (docs/APPROXIMATIONS.md item 6): PRINT-heavy programs
 * run slightly fast, delay loops slightly slow. Charging the full
 * PUMP_SLICE per statement, as before, made every statement cost
 * 4.17 ms and stretched MA2026's song-list phase to 10.1 s. */
#define STMT_CYCLES 2000L

static int in_run = 0;
static unsigned long last_cyc = 0;
static long vrtc_left = VRTC_PERIOD;
static long tb_left = 0;
static int vrtc_due = 0;                    /* tb_due: with tb_line above */
static long run_poll_left = PUMP_SLICE;     /* run-loop time to the next hal_poll */
static int slot_vrtc_seen = 0, slot_opn_seen = 0;

static long tb_period(void)
{
    /* Timer B period in 4 MHz CPU cycles: (256 - reg26) counts of
     * 1152 timer clocks at an effective Timer B clock of 3.9936 MHz.
     *
     * This follows QUASI88's calibrated interrupt model (src/intr.cpp:
     * sd_B_intr_base = TIMER_B_CONST 192 * sound_prescaler 6 *
     * (256 - reg) CPU cycles, sound clock default 3.9936 MHz), which the
     * user ear-verified as the genuine tempo on 2026-11-20. The previous
     * 7.9872 MHz assumption made Timer B-driven playback run at roughly
     * twice the intended tempo.
     *
     * Timing only: QUASI88's fmgen pitch/ADPCM clock is a separate fixed
     * 8 MHz, so opna_ymfm.cpp's CHIP_CLOCK stays 7987200 and must not be
     * changed to match this.
     *
     * The ratio is exact: 1152 * 4000000/3993600 == 15000/13, and the
     * reduced form keeps every intermediate within a 32-bit long. */
    return (long)(256 - opn_reg[0][0x26]) * 15000L / 13L;
}

/* Timer B started (reg 0x27 LOAD B 0 -> 1, see opn_write): the
 * counter begins a whole period. */
static void tb_restart(void)
{
    tb_left = tb_period();
}

/* Diagnostic snapshot for the regression suite (vessel_diag_*): the
 * delivery counts, and the emulated second at which the counters are
 * frozen. Emulated time, not wall time: a host samples the engine
 * thread at an arbitrary moment, the snapshot point is the same
 * instruction on every run. */
static unsigned long irq_tb_count = 0, irq_vrtc_count = 0;
static unsigned long frame_count = 0;   /* VRTC period wraps: the guest
                                         * frame clock (60 Hz), counted
                                         * whether or not VSYNC is unmasked */
unsigned long vessel_frame_count(void) { return frame_count; }
static long emu_sub = 0;                    /* cycles into the current second */
static long emu_secs = 0;                   /* whole emulated seconds this session */
static long diag_at = 0;                    /* snapshot second, 0 = off */
static int diag_taken = 0;
static VesselDiag diag;

static void diag_take(void)
{
    diag.tb_irqs = irq_tb_count;
    diag.vrtc_irqs = irq_vrtc_count;
    diag.b0_ssg = vessel_opn_class(0x00, 0x0F);
    diag.b0_rhythm = vessel_opn_class(0x10, 0x1F);
    diag.b0_mode = vessel_opn_class(0x20, 0x2F);
    diag.b0_fm = vessel_opn_class(0x30, 0xFF);
    diag.b0_keyon = opn_nwrite[0x28];
    diag.b0_tb = opn_nwrite[0x26];
    diag.b1_adpcm = vessel_opn_rhythm_writes();
    diag.b1_fm = vessel_opn_writes1() - diag.b1_adpcm;
    diag.adpcm_b_reads = opna_adpcm_b_ext_reads();
    diag_taken = 1;
}

void vessel_diag_snapshot_at(long emu_seconds)
{
    diag_at = emu_seconds > 0 ? emu_seconds : 0;
}

int vessel_diag_snapshot(VesselDiag *out)
{
    if (!diag_taken) return 0;
    *out = diag;
    return 1;
}

static void irq_advance(long cycles)
{
    opna_advance(cycles);               /* audio rendering follows the same clock */
    vrtc_left -= cycles;
    if (vrtc_left <= 0) {
        vrtc_left += VRTC_PERIOD;
        frame_count++;                  /* the CRT refresh runs regardless
                                         * of the guest's VSYNC mask */
        if (intr_mask & 0x02) vrtc_due = 1;     /* port E6: VSYNC enabled */
    }
    if (opn_reg[0][0x27] & 0x02) {              /* Timer B enabled */
        if (tb_left <= 0) tb_left = tb_period();
        tb_left -= cycles;
        if (tb_left <= 0) {
            tb_left += tb_period();
            /* The IRQ-enable bit (reg 0x27 bit 3) gates the flag
             * itself, as in QUASI88/ymfm: an overflow while the
             * guest has Timer B interrupts disabled leaves no
             * standing flag, so enabling the IRQ later cannot
             * deliver a stale request. */
            if (opn_reg[0][0x27] & 0x08) tb_flag = 1;
        }
    }
    tb_line_update();
    for (emu_sub += cycles; emu_sub >= 4000000L; emu_sub -= 4000000L) emu_secs++;
    if (diag_at && !diag_taken && emu_secs >= diag_at) diag_take();
}

/* Queue at most one interrupt with the CPU core, the higher priority
 * first: VSYNC outranks the sound interrupt on the genuine controller
 * (the same order as the port-E4 thresholds below), so when both are
 * latched VSYNC is delivered and Timer B stays latched for the next
 * call.
 * Deliveries are gated by the port-E4 priority level, as on hardware:
 * the sound interrupt needs level >= 5, VSYNC >= 2; a request whose
 * level is too low stays pending (the PIC latches it) until the level
 * rises. Both source masks act where the request is latched, not
 * here: SINTM (port 0x32 bit 7) gates the sound line ahead of the
 * latch (tb_line_update), and the port-E6 VSYNC mask drops a masked
 * VSYNC (irq_advance and the port write) instead of deferring it. */
static void irq_service(void)
{
    unsigned h;
    if (cpu.int_pending) return;
    if (vrtc_due && intr_level >= 2) {
        h = rd16((IRQ_PAGE << 8) | VRTC_VEC);
        if (h >= 0x8000) {
            vrtc_due = 0;
            if (!slot_vrtc_seen && tracing()) fprintf(stderr, "VESSEL VRTC int handler %04X\n", h);
            slot_vrtc_seen = 1;
            irq_vrtc_count++;
            z80_gen_int(&cpu, VRTC_VEC);
            return;
        }
        vrtc_due = 0;
    }
    if (tb_due && intr_level >= 5) {
        h = rd16((IRQ_PAGE << 8) | OPN_VEC);
        if (h >= 0x8000) {
            tb_due = 0;
            if (!slot_opn_seen && tracing()) fprintf(stderr, "VESSEL OPN int handler %04X\n", h);
            slot_opn_seen = 1;
            if (trace_sp() && mem_bank != 3)
                fprintf(stderr, "IRQFIRE bank=%d alu=%d pc=%04X\n",
                        mem_bank, vram_way(), (unsigned)cpu.pc);
            irq_tb_count++;
            z80_gen_int(&cpu, OPN_VEC);
            return;
        }
        tb_due = 0;
    }
}

/* Port E6 bit 1 cleared: drop the VSYNC request wherever it stands -
 * the controller latch, and a VRTC vector irq_service already queued
 * with the CPU core that the guest (under DI) has not accepted yet.
 * A queued Timer B vector is left alone. */
static void vrtc_retract(void)
{
    vrtc_due = 0;
    if (cpu.int_pending && cpu.int_data == VRTC_VEC) cpu.int_pending = 0;
}

/* The machine setup proper, shared by the lazy first-use path
 * (vessel_init) and the per-session reset (vessel_session_reset). */
static void vessel_setup(void)
{
    z80_init(&cpu);
    cpu.read_byte = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in = io_in;
    cpu.port_out = io_out;
    cpu.userdata = 0;
    /* PMD's init reads the I register to find the IM 2 vector table;
     * preset it (and IM 2) the way the host BASIC would. BASIC's VRTC
     * handler at 0xE808 gets an EI;RET stub for the handler chain. */
    cpu.i = IRQ_PAGE;
    cpu.interrupt_mode = 2;
    memset(gvram, 0, sizeof(gvram));
    mem_bank = 3;
    misc_port[0x34] = 0x77;             /* QUASI88 reset value */
    /* The genuine ROM leaves the analog-palette bit (0x32 bit 5) set
     * for the analog-display V2 environment; guest code preserves it
     * through read-modify-write port access, and the palette ports'
     * analog write protocol depends on it (QUASI88 pc88main.cpp). */
    misc_port[0x32] = 0x20;
    palette_reset();
    misc_port[0x35] = 0;
    misc_port[0x31] = 0;
    window_base = 0x8000;               /* identity, as booted BASIC leaves it */
    hal_poke(0, 0xE808, 0xFB);
    hal_poke(0, 0xE809, 0xC9);
    hal_poke(0, REST_CELL, 0xC9);
    /* The interpreter's evaluated-argument value lives at (0xEC41)
     * [lo] / (0xEC42) [hi]; the RST 8 trap also mirrors it into the
     * scratch block at 0xEC44 ([type][lo][hi]) that the trapped ROM
     * readers (0x18A3 / 0xA794) fetch from. */
    hal_poke(0, 0xEC41, 0);
    hal_poke(0, 0xEC42, 0);
    hal_poke(0, 0xEC44, 0);
    hal_poke(0, 0xEC45, 0);
    hal_poke(0, 0xEC46, 0);
}

/* Idempotent: safe to call mid-run, it never resets a guest that is
 * already set up. The first call of a session also switches the
 * per-statement pump on (pump_live) - emulated time, and with it
 * audio rendering and the engine's pacing, starts at the first
 * machine-code use, exactly as it did when this call also performed
 * the setup. */
void vessel_init(void)
{
    pump_live = 1;
    if (inited) return;
    inited = 1;
    vessel_setup();
}

/* Per-session reset for hosts that boot more than one engine in a
 * process (engine.c p8_start): every guest-visible static goes back
 * to its power-on value and the setup runs again, so the new
 * session cannot see the previous one's RAM-side stubs, VRAM, CPU,
 * ports or OPNA shadow. The disk image is NOT dropped here: the
 * engine's disk is bound at p8_create() (vessel_forget_disk), and
 * the image p8_boot's materialization opened from it stays open for
 * the run. Must run AFTER halp8_session_reset():
 * vessel_setup pokes its stubs (0xE808, REST_CELL, 0xEC41..) into
 * the guest RAM that call clears. `inited` is left set, so the lazy
 * vessel_init() calls during the run do not set the guest up a
 * second time. Not reset: the trace-only state (vtrace, rom_seen,
 * pmd_seen, in_seen, out_seen, pc_ring, and the function-local
 * trace counters), which the guest cannot observe. */
void vessel_session_reset(void)
{
    pmd_loaded = 0;
    neomsc_loaded = 0;
    relay_warned = 0;
    /* disk subsystem command processor */
    oq_head = oq_count = 0;
    read_len = 0; read_valid = 0;
    memset(read_buf, 0, sizeof(read_buf));
    memset(write_buf, 0, sizeof(write_buf));
    sub_cmd = -1; sub_need = 0; sub_npar = 0; sub_wleft = 0;
    memset(sub_par, 0, sizeof(sub_par));
    sub_err = 0; surface_mode = 0;
    /* 8255 handshake */
    cfg_a_input = 1; cfg_b_input = 0; ppi_mode = 0;
    hostc = 0; prev_c = 0; latch = 0xFF; latched = 0; sub_dac = 0;
    /* ports, banking, ALU (idle = main RAM, latch empty), kanji ROM */
    /* Plain memset, including the pile byte [0x53] that is otherwise
     * only touched through pile_store/pile_load: the session reset
     * runs from p8_start before the engine thread is created, so no
     * engine thread exists to write the byte concurrently. */
    memset(misc_port, 0, sizeof(misc_port));
    kanji_addr[0] = kanji_addr[1] = 0;
    intr_level = 7;
    intr_mask = 0xFF;
    alu_buf = 0;
    /* OPNA register shadow, Timer B, write counters */
    opn_addr[0] = opn_addr[1] = 0;
    memset(opn_reg, 0, sizeof(opn_reg));
    memset(opn_nwrite, 0, sizeof(opn_nwrite));
    memset(opn_nwrite1, 0, sizeof(opn_nwrite1));
    tb_flag = 0;
    tb_line = 0;
    /* interrupt model. in_run must be cleared: a stop request taken
     * by run_poll's hal_poll bails out (p8_bail's longjmp) from
     * inside run_loop, which leaves it set and would keep vessel_pump
     * off for the whole next session. */
    in_run = 0;
    last_cyc = 0;
    vrtc_left = VRTC_PERIOD; tb_left = 0;
    vrtc_due = 0; tb_due = 0;
    run_poll_left = PUMP_SLICE;
    slot_vrtc_seen = 0; slot_opn_seen = 0;
    /* diagnostics: counts, emulated clock and the snapshot restart
     * with the session; the armed snapshot second (diag_at) is the
     * host's setting and stays */
    irq_tb_count = 0; irq_vrtc_count = 0; frame_count = 0;
    emu_sub = 0; emu_secs = 0;
    diag_taken = 0;
    xde_img_warned = 0;
    /* CPU, GVRAM, bank, port defaults, palette, window, RAM stubs */
    pump_live = 0;
    inited = 1;
    vessel_setup();
}

static unsigned short pc_ring[64];
static int pc_ring_pos;

/* HALT = wait for an interrupt. Instead of stepping the halted CPU's
 * NOPs one by one, emulated time jumps to the next timer event (the
 * VRTC tick, or the Timer B overflow when Timer B runs) and goes
 * through the same irq_advance / irq_service accounting as executed
 * code, so the VRTC cadence, tb_left and audio rendering stay on one
 * clock. Jumps are rounded up to 4 T-states, the halted CPU's NOP
 * granularity. Returns 1 once an interrupt is queued (the next
 * z80_step accepts it and execution resumes after the HALT), 0 when
 * the HALT cannot end: interrupts are disabled (there is no NMI
 * source), or nothing became deliverable within HALT_WAIT_CAP cycles
 * of emulated time (level too low, source masked, Timer B stopped,
 * empty handler slot). The caller then stops the run, as every HALT
 * did before.
 *
 * Halted time is charged to the run loop's poll budget (run_poll)
 * like executed time. */
#define HALT_WAIT_CAP 4000000L      /* 1 s of emulated time = 60 VRTC ticks */

/* Time the run loop consumes - executed instructions and HALT
 * fast-forward alike - is emulated time passing inside one
 * machine-code call, where exec_loop's per-statement poll never runs:
 * the same situation as vessel_burn's sector wait, and handled the
 * same way. Every cycle is charged to one budget and the HAL gets its
 * poll once per PUMP_SLICE of it (counted across calls, so an EI;HALT
 * loop is covered whether its interrupts end the HALT in halt_wait or
 * are accepted inside the HALT's own step), which paces the embedded
 * engine against the audio clock and lets a stop or pause request
 * land. Call it with the interrupt state settled (after irq_service):
 * the poll may not return (p8_bail). */
static void run_poll(long cycles)
{
    for (run_poll_left -= cycles; run_poll_left <= 0; run_poll_left += PUMP_SLICE)
        hal_poll();
}

/* One CPU step with its time accounting: emulated time follows the
 * CPU's own cycle counter. */
static void cpu_step(void)
{
    unsigned long now;
    long dc;
    z80_step(&cpu);
    now = cpu.cyc;
    dc = (long)(now - last_cyc);
    last_cyc = now;
    if (dc > 0) irq_advance(dc);
    irq_service();
    if (dc > 0) run_poll(dc);
}

static int halt_wait(void)
{
    long waited = 0;
    if (!cpu.iff1 && !cpu.iff_delay) return 0;
    while (!cpu.int_pending) {
        long dt = vrtc_left;
        if (waited >= HALT_WAIT_CAP) return 0;
        if (opn_reg[0][0x27] & 0x02) {          /* Timer B running */
            long tb = tb_left > 0 ? tb_left : tb_period();
            if (tb < dt) dt = tb;
        }
        if (dt < 4) dt = 4;
        dt = (dt + 3) & ~3L;
        cpu.cyc += (unsigned long)dt;
        last_cyc = cpu.cyc;
        irq_advance(dt);
        irq_service();
        waited += dt;
        run_poll(dt);
    }
    return 1;
}

/* P8P_TRACE_SP diagnostics for the instruction about to execute; the
 * run loop calls this only with that trace on, so none of it - the
 * NEOMSC stack checkpoints (CG loader 0xA4A9 / 0xA4D6 / 0xA4EA /
 * 0xA566; 0x8F03 from the same investigation), the recent-PC ring
 * and the OPN-handler entry detection - costs anything otherwise. */
static void trace_sp_step(void)
{
    unsigned h;
    if (cpu.pc == 0xA4A9 || cpu.pc == 0xA4EA
        || cpu.pc == 0xA566 || cpu.pc == 0xA4D6 || cpu.pc == 0x8F03)
        fprintf(stderr, "SPCHK pc=%04X sp=%04X top=%04X iy=%04X\n",
                (unsigned)cpu.pc, (unsigned)cpu.sp, rd16(cpu.sp),
                (unsigned)cpu.iy);
    pc_ring[pc_ring_pos & 63] = (unsigned short)cpu.pc; pc_ring_pos++;
    h = rd16((IRQ_PAGE << 8) | OPN_VEC);
    if (cpu.pc == h && h >= 0x8000 && pc_ring_pos >= 2
        && pc_ring[(pc_ring_pos - 2) & 63] != (unsigned short)h)
        fprintf(stderr, "IRQACPT from=%04X bank=%d alu=%d\n",
                pc_ring[(pc_ring_pos - 2) & 63], mem_bank, vram_way());
}

static long run_loop_body(void)
{
    long n;
    int t;
    int steps = getenv("P8P_TRACE_STEPS") != 0;
    int tsp = trace_sp();
    last_cyc = cpu.cyc;
    for (n = 0; n < STEP_CAP; n++) {
        t = trap();
        if (t < 0) return n;
        if (t > 0) continue;
        if (steps) fprintf(stderr, "STEP %04X %02X SP=%04X\n", (unsigned)cpu.pc, hal_peek(0, cpu.pc), (unsigned)cpu.sp);
        if (tsp) trace_sp_step();
        cpu_step();
        if (cpu.halted) {
            if (!halt_wait()) {
                fprintf(stderr, "VESSEL HALT at %04X with no deliverable"
                        " interrupt, stopped\n", (unsigned)cpu.pc);
                return n;
            }
            /* The wakeup vector is queued but the CPU is still halted
             * with PC at the continuation address. Let the core accept
             * it now, before the next iteration's trap() looks at that
             * PC: a HALT at 0xFFFE continues at the sentinel, and
             * trap() would end the run with the vector still pending.
             * (More than one step only while an EI delay runs out;
             * halt_wait returned 1, so acceptance is certain.) */
            while (cpu.halted && cpu.int_pending) cpu_step();
        }
    }
    fprintf(stderr, "VESSEL step cap reached at %04X, stopped"
            " [A=%02X HL=%04X DE=%04X BC=%04X SP=%04X]\n",
            (unsigned)cpu.pc, cpu.a,
            (unsigned)((cpu.h << 8) | cpu.l),
            (unsigned)((cpu.d << 8) | cpu.e),
            (unsigned)((cpu.b << 8) | cpu.c), (unsigned)cpu.sp);
    if (trace_sp()) {   /* recent-PC ring, for spin diagnosis */
        int i;
        fprintf(stderr, "RECENT:");
        for (i = 0; i < 64; i++)
            fprintf(stderr, " %04X", pc_ring[(pc_ring_pos + i) & 63]);
        fprintf(stderr, "\n");
    }
    return -1;
}

static long run_loop(void)
{
    long r;
    in_run = 1;
    r = run_loop_body();
    in_run = 0;
    return r;
}

/* Advance emulated time while the native interpreter runs (BASIC is
 * the mainline; the Z80 only runs in bursts), and execute any
 * interrupt handlers that come due, on a sentinel stack like a call.
 * `cycles` is the emulated time this call accounts for. */
static void pump_advance(long cycles)
{
    int k;
    static unsigned long pump_count = 0;
    if (!pump_live || in_run) return;
    /* At rest the machine belongs to the host mainline, which runs
     * with interrupts enabled (the ROM does); a driver call that
     * returns with DI must not mute the machine for good. */
    cpu.iff1 = 1;
    cpu.iff2 = 1;
    /* At rest the C000-FFFF window likewise shows main RAM again:
     * the interpreter's own work areas live there, so the genuine
     * machine is never left with a VRAM plane selected between
     * statements. Driver exits do not guarantee it — NEOMSC's CG
     * decompressor, for one, leaves the G plane selected on its
     * normal (0x00-terminated) exit; its OUT (0x5F) sits only on
     * the page-exhaustion path. Without this, PMD's sequencer reads
     * its song data at 0xCA00 through the stale plane and playback
     * goes silent (found 2026-11-18 in the engine build, where the
     * XDD draws actually execute). */
    mem_bank = 3;
    if (tracing() && ++pump_count % 60000 == 0) {
        unsigned long tot = 0;
        int i;
        for (i = 0; i < 256; i++) tot += opn_nwrite[i];
        fprintf(stderr, "VESSEL pump %lu: OPN writes total %lu, keyon/off %lu, timerB %lu, iff1 %d pend %d\n",
                pump_count, tot, opn_nwrite[0x28], opn_nwrite[0x26],
                (int)cpu.iff1, (int)cpu.int_pending);
    }
    irq_advance(cycles);
    /* Step the CPU whenever an interrupt is due OR already pending:
     * a vector queued at the tail of a driver call is only accepted
     * by a later step, so refusing to step while one is pending
     * would wedge playback for good. */
    for (k = 0; k < 2 && (vrtc_due || tb_due || cpu.int_pending); k++) {
        cpu.halted = 0;
        cpu.sp = (uint16_t)(STACK_INIT - 2);
        hal_poke(0, cpu.sp, SENTINEL & 0xFF);
        hal_poke(0, cpu.sp + 1, SENTINEL >> 8);
        cpu.pc = (uint16_t)REST_CELL;   /* steppable rest PC: a pending
                                         * interrupt is accepted before
                                         * its RET reaches the sentinel */
        irq_service();
        run_loop();
        if (!cpu.int_pending && !vrtc_due && !tb_due) break;
    }
}

/* One full slice (PUMP_SLICE): for time that passes in bulk inside a
 * single statement (vessel_burn's sector wait). */
void vessel_pump(void)
{
    pump_advance(PUMP_SLICE);
}

/* One BASIC statement's modelled cost (STMT_CYCLES). Called on every
 * pass of exec_loop, so the machine advances at the interpreter's
 * pace while BASIC runs. */
void vessel_statement(void)
{
    pump_advance(STMT_CYCLES);
}

void vessel_exec(unsigned entry)
{
    long n;
    vessel_init();
    cpu.halted = 0;
    cpu.sp = (uint16_t)(STACK_INIT - 2);
    hal_poke(0, cpu.sp, SENTINEL & 0xFF);
    hal_poke(0, cpu.sp + 1, SENTINEL >> 8);
    cpu.pc = (uint16_t)(entry & 0xFFFF);
    if (tracing()) fprintf(stderr, "VESSEL exec %04X\n", entry & 0xFFFF);
    n = run_loop();
    if (tracing() && n >= 0) fprintf(stderr, "VESSEL returned after %ld steps\n", n);
}

/* Call a driver entry with preset registers (hook experiments and,
 * later, the extension statement relay). Returns the step count. */
long vessel_call(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de)
{
    vessel_init();
    cpu.halted = 0;
    cpu.sp = (uint16_t)(STACK_INIT - 2);
    hal_poke(0, cpu.sp, SENTINEL & 0xFF);
    hal_poke(0, cpu.sp + 1, SENTINEL >> 8);
    cpu.a = (uint8_t)a;
    cpu.h = (uint8_t)(hl >> 8); cpu.l = (uint8_t)hl;
    cpu.b = (uint8_t)(bc >> 8); cpu.c = (uint8_t)bc;
    cpu.d = (uint8_t)(de >> 8); cpu.e = (uint8_t)de;
    cpu.pc = (uint16_t)(entry & 0xFFFF);
    if (tracing()) fprintf(stderr, "VESSEL call %04X A=%02X HL=%04X BC=%04X DE=%04X\n",
                               entry & 0xFFFF, a & 0xFF, hl & 0xFFFF, bc & 0xFFFF, de & 0xFFFF);
    return run_loop();
}

/* Call an entry whose return sequence pops one extra word into HL
 * before its RET (the Disk BASIC statement-dispatch stack shape:
 * the caller pushes the token-stream pointer, the handler's tail
 * POP HL / RET consumes it). `pushed` is that word. */
long vessel_call_pushed(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de, unsigned pushed)
{
    vessel_init();
    cpu.halted = 0;
    cpu.sp = (uint16_t)(STACK_INIT - 4);
    hal_poke(0, cpu.sp, pushed & 0xFF);
    hal_poke(0, cpu.sp + 1, (pushed >> 8) & 0xFF);
    hal_poke(0, cpu.sp + 2, SENTINEL & 0xFF);
    hal_poke(0, cpu.sp + 3, SENTINEL >> 8);
    cpu.a = (uint8_t)a;
    cpu.h = (uint8_t)(hl >> 8); cpu.l = (uint8_t)hl;
    cpu.b = (uint8_t)(bc >> 8); cpu.c = (uint8_t)bc;
    cpu.d = (uint8_t)(de >> 8); cpu.e = (uint8_t)de;
    cpu.pc = (uint16_t)(entry & 0xFFFF);
    if (tracing()) fprintf(stderr, "VESSEL callp %04X A=%02X HL=%04X BC=%04X DE=%04X pushed=%04X\n",
                               entry & 0xFFFF, a & 0xFF, hl & 0xFFFF, bc & 0xFFFF, de & 0xFFFF, pushed & 0xFFFF);
    return run_loop();
}

/* Emulated time a native XDE transfer spends per 256-byte sector.
 * On the genuine machine the CPU waits in NEOMSC's subsystem poll
 * while a 2D floppy delivers roughly 30 sectors a second (seek +
 * rotational latency), and the interrupt handlers — the BGM
 * sequencer above all — keep running throughout the wait. The
 * native equivalents therefore burn the same time sector by
 * sector through the pump, which delivers due handlers as it
 * goes. Without this, MA2026's loading BGM (started just before
 * the 448-sector ADPCM load) collapses to a fraction of a second
 * and the whole boot sequence runs in the wrong order of
 * durations (measured against QUASI88: BGM audible ~19 s). */
#define XDE_SECTOR_CYCLES 133333L   /* 4 MHz / 30 sectors per sec */

static void vessel_burn(long cycles)
{
    while (cycles > 0) {
        vessel_pump();
        /* The burn is emulated time passing inside a single
         * statement, where exec_loop's per-statement pacing never
         * runs: give the HAL its poll so a pacing host (the
         * embedded engine) can hold the audio lead against the
         * wall clock instead of flooding its ring buffer and
         * dropping the very music this wait exists to let play.
         * The host harness's poll is a no-op (offline rendering). */
        hal_poll();
        cycles -= PUMP_SLICE;
    }
}

/* The XDE services' disk argument must name an image the open D88
 * file holds. A request for any other image reads nothing (the
 * services return 0 sectors, as for an unreadable first sector) and
 * is reported once per session even with tracing off, so a title
 * asking for its second disk on a one-image file does not go on
 * silently with whatever was in memory. With no disk open at all
 * the read fails the same way, without the message. */
static int xde_image_ok(int img)
{
    if (!d88_open()) return 0;
    if (d88_base(img) >= 0) return 1;
    if (!xde_img_warned) {
        xde_img_warned = 1;
        fprintf(stderr, "VESSEL XDE: disk image %d is not in the disk"
                " file (%d image%s), nothing read\n",
                img, d88_image_count, d88_image_count == 1 ? "" : "s");
    }
    return 0;
}

/* Text-VRAM cell of NEOMSC's service-1 track countdown: row 0,
 * column 78 of the guest text image at 0xF3C8 (two cells). */
#define XDE_COUNT_CELL 0xF416

/* FFDE service 1 equivalent: bulk-load `tracks` logical tracks
 * starting at logical track `ltrack` from disk image `img` of the
 * P8P_DISK file into the OPNA's ADPCM-B sample DRAM (the Sound
 * Board II's 256 KiB DRAM that PMD's PCM part plays from through
 * the bank-1 ADPCM-B registers: control 00H, delta-N pitch),
 * starting at DRAM offset 0. The ADPCM-A side is the chip's rhythm
 * ROM and is not a load target. Returns the number of sectors
 * stored in DRAM: a load past the 256 KiB capacity is still read
 * and timed like the genuine burst, but the excess is dropped.
 * Like the genuine loop it also shows the track countdown at
 * XDE_COUNT_CELL (docs/APPROXIMATIONS.md item 30). */
int vessel_dram_load(int img, int ltrack, int tracks)
{
    unsigned char buf[256];
    int n = 0, stored = 0, t, r, b;
    unsigned long dst = 0;
    int left = tracks;      /* NEOMSC's remaining-track counter */
    if (!xde_image_ok(img)) return 0;
    for (t = ltrack; left > 0; t++, left--) {
        /* NEOMSC's transfer loop shows its progress: before each
         * track it writes (remaining - 1) as two uppercase hex
         * digits into the text VRAM at 0xF416/0xF417 (row 0,
         * columns 78-79), so RT=28 counts "1B" down to "00" and the
         * "00" stays until the program redraws row 0. Same
         * formatter as the genuine one: nibble, +7 if >= 10, +'0'.
         * Guest RAM, so it goes through hal_poke like every other
         * vessel write (the engine merges the image for display). */
        if (left >= 1) {
            int v = (left - 1) & 0xFF, hi = v >> 4, lo = v & 0x0F;
            hal_poke(0, XDE_COUNT_CELL,     (hi >= 10 ? hi + 7 : hi) + 0x30);
            hal_poke(0, XDE_COUNT_CELL + 1, (lo >= 10 ? lo + 7 : lo) + 0x30);
        }
        for (r = 1; r <= 16; r++) {
            if (!d88_read_sector_img(img, t, r, buf)) return stored;
            if (dst < 256UL * 1024) stored++;
            for (b = 0; b < 256; b++) opna_adpcm_b_write(dst++, buf[b]);
            n++;
            vessel_burn(XDE_SECTOR_CYCLES);
        }
    }
    if (tracing()) fprintf(stderr, "VESSEL dram_load ltrack %d tracks %d (%d sectors)\n",
                           ltrack, tracks, n);
    return stored;
}

/* FFDE service 0 equivalent: read count 256-byte sectors starting at
 * (logical track, sector) from disk image `img` of the P8P_DISK
 * file into guest memory. Logical track = cylinder * 2 + head,
 * sector = 1..16 (D88 order). Returns the number of sectors read. */
int vessel_disk_read(int img, int ltrack, int sector, int count, unsigned addr)
{
    unsigned char buf[256];
    int i, t = ltrack, r = sector;
    if (!xde_image_ok(img)) return 0;
    for (i = 0; i < count; i++) {
        int j;
        if (!d88_read_sector_img(img, t, r, buf)) break;
        for (j = 0; j < 256; j++) hal_poke(0, (addr + (unsigned)i * 256u + (unsigned)j) & 0xFFFF, buf[j]);
        if (++r > 16) { r = 1; t++; }
        vessel_burn(XDE_SECTOR_CYCLES);
    }
    if (tracing()) fprintf(stderr, "VESSEL disk_read track %d sec %d count %d -> %04X (%d read)\n",
                           ltrack, sector, count, addr, i);
    if (i > 0) vessel_notify_load(addr, (long)i * 256L);
    return i;
}

unsigned long vessel_opn_keyons(void)
{
    return opn_nwrite[0x28];
}

unsigned long vessel_opn_writes(void)
{
    unsigned long tot = 0;
    int i;
    for (i = 0; i < 256; i++) tot += opn_nwrite[i];
    return tot;
}

unsigned long vessel_opn_writes1(void)
{
    unsigned long tot = 0;
    int i;
    for (i = 0; i < 256; i++) tot += opn_nwrite1[i];
    return tot;
}

unsigned long vessel_opn_adpcmb_ctl(void)
{
    return opn_nwrite[0x10];    /* ADPCM-B control register writes */
}

unsigned long vessel_opn_class(int lo, int hi)
{
    unsigned long tot = 0;
    int i;
    for (i = lo; i <= hi && i < 256; i++) tot += opn_nwrite[i];
    return tot;
}

unsigned long vessel_opn_rhythm_writes(void)
{
    unsigned long tot = 0;
    int i;
    for (i = 0; i < 0x30; i++) tot += opn_nwrite1[i];   /* ADPCM-A block */
    return tot;
}

void vessel_notify_load(unsigned addr, long len)
{
    addr &= 0xFFFF;
    if (len > 0 && addr <= PMD_START && (long)addr + len > (long)PMD_START) pmd_loaded = 1;
    /* NEOMSC detection: see the predicate's description at neomsc_sig. */
    if (len > 0 && addr <= NEOMSC_END && (long)addr + len > (long)NEOMSC_START) {
        unsigned k = 0;
        if (addr == NEOMSC_START && len == (long)(NEOMSC_END - NEOMSC_START + 1))
            for (; k < sizeof(neomsc_sig); k++)
                if (hal_peek(0, NEOMSC_START + k) != neomsc_sig[k]) break;
        neomsc_loaded = (k == sizeof(neomsc_sig));
    }
    if (tracing()) fprintf(stderr, "VESSEL load %04X len %ld%s%s\n", addr, len,
                           pmd_loaded ? " (PMD loaded)" : "",
                           neomsc_loaded ? " (NEOMSC detected)" : "");
}

/* Resolve an extension-statement relay: the statement's handler is
 * whatever the guest planted in its EEA vector (0xEEA1 FFDD, 0xEEA4
 * FFDE, 0xEEA7 FFDF), so the target is read from the vector's JP
 * instead of being assumed. Returns 1 with *target set when the
 * vector holds JP nn (0xC3) to RAM (nn >= 0x8000). That is a RAM-only
 * policy: the vessel carries no ROM image, only native stand-ins for
 * a few specific ROM entries, so a vector aimed below 0x8000 is not
 * treated as a handler; the all-ones sentinel address cannot be one
 * either. Otherwise returns 0 and the caller
 * makes the statement a no-op for this call; the first failure per
 * vector in a session is reported on stderr even with tracing off, so
 * an unhooked statement does not fail silently. `name` is the
 * statement's name for that message. */
int vessel_relay_target(unsigned vector, const char *name, unsigned *target)
{
    unsigned t = rd16(vector + 1);
    unsigned char bit;
    if (hal_peek(0, vector & 0xFFFF) == 0xC3 && t >= 0x8000 && t != SENTINEL) {
        *target = t;
        return 1;
    }
    bit = (unsigned char)(vector == 0xEEA1 ? 1 : vector == 0xEEA4 ? 2 : 4);
    if (!(relay_warned & bit)) {
        relay_warned |= bit;
        fprintf(stderr, "VESSEL %s: no handler hooked at vector %04X"
                " (bytes %02X %02X %02X), statement ignored\n",
                name, vector & 0xFFFF, hal_peek(0, vector & 0xFFFF),
                t & 0xFF, t >> 8);
    }
    return 0;
}
