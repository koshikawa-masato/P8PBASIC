/* engine.c - see engine.h. The interpreter's exit path is the
 * engine's stop path: when a program run finishes, exec.c calls
 * hal_term() and then exit(0); halp8's hal_term longjmps back here
 * instead (p8internal.h), so the thread ends cleanly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "basic.h"
#include "engine.h"
#include "halp8.h"
#include "vessel.h"
#include "p8internal.h"
#include "materialize.h"
#include "opna.h"

void interp_init(void);
void exec_direct_pending_set(const char *s);

jmp_buf p8_bail_jmp;
volatile int p8_bail_armed = 0;

void p8_bail(void)
{
    if (p8_bail_armed) longjmp(p8_bail_jmp, 1);
}

#define RING_FRAMES (44100 * 2)     /* 2 seconds of stereo audio */
struct P8Engine {
    char *d88_path;
    char *workdir;
    char bootfile[32];
    pthread_t th;
    volatile int running;
    int started;
    short ring[RING_FRAMES * 2];
    int ring_head, ring_tail;
    unsigned long dropped;
    unsigned long rendered;         /* all frames the sink ever saw */
    struct timespec t0;             /* p8_start wall clock (under ring_mu) */
    long cpu_clock;                 /* 4000000 or 8000000 Hz, applied at p8_start */
    char *rhythm_dir;               /* external rhythm samples, applied at p8_start */
    pthread_mutex_t ring_mu;        /* guards the ring and t0 */
};

static P8Engine *g_engine = 0;      /* single instance, see engine.h */

double p8_audio_seconds(void)
{
    if (!g_engine) return 0;
    return (double)g_engine->rendered / 44100.0;
}

double p8_wall_seconds(void)
{
    struct timespec ts, t0;
    if (!g_engine) return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* t0 is two words and p8_set_paused() moves it from the host
     * thread: copy it under the lock so both fields come from the
     * same update */
    pthread_mutex_lock(&g_engine->ring_mu);
    t0 = g_engine->t0;
    pthread_mutex_unlock(&g_engine->ring_mu);
    return (double)(ts.tv_sec - t0.tv_sec) +
           (double)(ts.tv_nsec - t0.tv_nsec) / 1e9;
}

static void ring_sink(const short *frames, int count, void *ctx)
{
    P8Engine *e = (P8Engine *)ctx;
    int i;
    pthread_mutex_lock(&e->ring_mu);
    e->rendered += (unsigned long)count;
    for (i = 0; i < count; i++) {
        int next = (e->ring_tail + 1) % RING_FRAMES;
        if (next == e->ring_head) { e->dropped++; continue; }
        e->ring[e->ring_tail * 2] = frames[i * 2];
        e->ring[e->ring_tail * 2 + 1] = frames[i * 2 + 1];
        e->ring_tail = next;
    }
    pthread_mutex_unlock(&e->ring_mu);
}

int p8_audio_read(P8Engine *e, short *out, int frames)
{
    int n = 0;
    pthread_mutex_lock(&e->ring_mu);
    while (n < frames && e->ring_head != e->ring_tail) {
        out[n * 2] = e->ring[e->ring_head * 2];
        out[n * 2 + 1] = e->ring[e->ring_head * 2 + 1];
        e->ring_head = (e->ring_head + 1) % RING_FRAMES;
        n++;
    }
    pthread_mutex_unlock(&e->ring_mu);
    return n;
}

unsigned long p8_audio_dropped(P8Engine *e) { return e->dropped; }

unsigned long p8_frame_count(P8Engine *e)
{
    (void)e;
    return vessel_frame_count();
}

P8Engine *p8_create(const char *d88_path)
{
    P8Engine *e = (P8Engine *)calloc(1, sizeof(P8Engine));
    if (!e) return 0;
    e->d88_path = strdup(d88_path);
    e->cpu_clock = 4000000L;
    g_engine = e;
    pthread_mutex_init(&e->ring_mu, 0);
    /* the vessel opens the disk lazily from the environment */
    setenv("P8P_DISK", d88_path, 1);
    /* a new engine binds a new disk: drop the image a previous
     * engine left open, before p8_boot reads sectors through it */
    vessel_forget_disk();
    opna_reset();
    opna_set_sink(ring_sink, e);
    return e;
}

void p8_destroy(P8Engine *e)
{
    if (!e) return;
    if (e->started) p8_stop(e);
    opna_set_sink(0, 0);
    free(e->d88_path);
    free(e->workdir);
    free(e->rhythm_dir);
    free(e);
}

/* Write the disk's files into a fresh scratch directory (shared
 * with the host CLI: src/materialize.c). Returns 0 and sets
 * e->workdir / e->bootfile on success. */
static int materialize(P8Engine *e)
{
    char *dir = p8_materialize(e->bootfile, sizeof(e->bootfile));
    if (!dir) return -1;
    e->workdir = dir;
    return 0;
}

int p8_boot(P8Engine *e)
{
    if (materialize(e) != 0) return -1;
    if (chdir(e->workdir) != 0) return -1;
    return 0;
}

static void *engine_thread(void *arg)
{
    P8Engine *e = (P8Engine *)arg;
    interp_init();
    hal_init(0);
    scr_init();
    g_init();
    exit_after_run = 1;
    ok_needed = 0;
    if (setjmp(p8_bail_jmp) == 0) {
        p8_bail_armed = 1;
        if (!prog_load(e->bootfile, 0)) {
            p8_bail_armed = 0;
            e->running = 0;
            return 0;
        }
        exec_direct_pending_set("RUN");
        top_level();            /* leaves via hal_term -> p8_bail */
    }
    p8_bail_armed = 0;
    e->running = 0;
    return 0;
}

void p8_set_cpu_clock(P8Engine *e, long hz)
{
    if (!e) return;
    if (hz == 4000000L || hz == 8000000L) e->cpu_clock = hz;
}

void p8_set_rhythm_dir(P8Engine *e, const char *dir)
{
    if (!e) return;
    free(e->rhythm_dir);
    e->rhythm_dir = dir ? strdup(dir) : 0;
}

void p8_start(P8Engine *e)
{
    if (e->started) return;
    e->running = 1;
    e->started = 1;
    /* Session boundary: no decoder state or rendered audio of an
     * earlier session of this engine carries over. Before the clock
     * and the rhythm load, which it would otherwise undo. */
    opna_reset();
    /* The CPU clock is a per-session setting: apply it before the
     * resets below so they initialise clock-scaled state, and
     * unconditionally so a 4 MHz session after an 8 MHz one in the
     * same process does not inherit the faster clock. */
    vessel_set_clock_mult(e->cpu_clock == 8000000L ? 2 : 1);
    opna_set_cpu_clock(e->cpu_clock);
    /* Likewise per session: a session without a rhythm dir never
     * plays an earlier session's samples. */
    if (e->rhythm_dir) opna_load_rhythm_dir(e->rhythm_dir);
    else opna_clear_rhythm();
    /* A process may boot several engines in turn: put the HAL and
     * the vessel back to power-on state. Order matters - the
     * vessel's setup pokes its stubs into the guest RAM that
     * halp8_session_reset() clears. */
    halp8_session_reset();
    vessel_session_reset();
    clock_gettime(CLOCK_MONOTONIC, &e->t0);
    pthread_create(&e->th, 0, engine_thread, e);
}

void p8_stop(P8Engine *e)
{
    if (!e->started) return;
    halp8_request_stop();
    pthread_join(e->th, 0);
    e->started = 0;
    e->running = 0;
}

int p8_running(P8Engine *e) { return e->running; }

void p8_set_paused(P8Engine *e, int paused)
{
    static struct timespec pause_begin;
    if (paused && !halp8_paused()) {
        clock_gettime(CLOCK_MONOTONIC, &pause_begin);
        halp8_set_paused(1);
    } else if (!paused && halp8_paused()) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        /* shift the pacing origin past the pause, so the guest
         * does not fast-forward to catch up with the wall clock */
        pthread_mutex_lock(&e->ring_mu);    /* the engine thread reads t0 */
        e->t0.tv_sec += now.tv_sec - pause_begin.tv_sec;
        e->t0.tv_nsec += now.tv_nsec - pause_begin.tv_nsec;
        if (e->t0.tv_nsec >= 1000000000L) { e->t0.tv_nsec -= 1000000000L; e->t0.tv_sec++; }
        if (e->t0.tv_nsec < 0) { e->t0.tv_nsec += 1000000000L; e->t0.tv_sec--; }
        pthread_mutex_unlock(&e->ring_mu);
        halp8_set_paused(0);
    }
}
int  p8_paused(P8Engine *e) { (void)e; return halp8_paused(); }

void p8_push_key(P8Engine *e, int code) { (void)e; halp8_push_key(code); }
void p8_set_matrix(P8Engine *e, int idx, unsigned char v)
{
    (void)e;
    halp8_set_matrix(idx, v);
}

void p8_frame(P8Engine *e, unsigned char *rgba)
{
    static const unsigned char blank_gfx[400 * 640];
    const unsigned char *g = halp8_gfx_visible()
        ? halp8_gfx(halp8_gfx_display_page()) : blank_gfx;
    const unsigned char *vp[3];
    int x, y, gfx_on = halp8_gfx_visible();
    /* Port 0x53 bits 1-3: graphics planes taken out of the display.
     * Applied once, to the final colour index, AFTER the source
     * (vessel pixel or interpreter pixel) has been chosen: masking
     * the vessel pixel first would turn a hidden pixel into "no
     * vessel pixel" and let the interpreter's stale pixel underneath
     * show through. With the mask 0 this is the pre-pile path. */
    int pile_mask = vessel_pile_plane_mask();
    (void)e;
    vp[0] = vessel_vram_plane(0);       /* B */
    vp[1] = vessel_vram_plane(1);       /* R */
    vp[2] = vessel_vram_plane(2);       /* G */
    for (y = 0; y < 400; y++) {
        for (x = 0; x < 640; x++) {
            int idx = g[y * 640 + x], r, gg, b;
            unsigned char *p = rgba + (y * 640 + x) * 4;
            if (gfx_on && vp[0]) {
                /* Vessel VRAM: 80 bytes x 200 lines per plane, shown
                 * line-doubled. Where the guest drew through the
                 * C000 window it wins over the interpreter's own
                 * graphics plane (on hardware they are one memory). */
                int off = (y >> 1) * 80 + (x >> 3);
                int bit = 7 - (x & 7);
                int vi = ((vp[0][off] >> bit) & 1)
                       | (((vp[1][off] >> bit) & 1) << 1)
                       | (((vp[2][off] >> bit) & 1) << 2);
                if (vi) idx = vi;
            }
            if (pile_mask) idx &= ~pile_mask;
            if (idx < 8) vessel_palette(idx, &r, &gg, &b);
            else halp8_palette(idx, &r, &gg, &b);
            p[0] = (unsigned char)r;
            p[1] = (unsigned char)gg;
            p[2] = (unsigned char)b;
            p[3] = 255;
        }
    }
}

/* --- text screen merge -------------------------------------------------
 * On hardware there is ONE text screen: the memory image at 0xF3C8
 * (25 rows x 120 bytes: 80 code bytes, then 20 (column, attr)
 * attribute pairs). Here the interpreter keeps its own cell model
 * (halp8's tv/ta) while guest machine code (NEOMSC) writes the
 * image directly - the staff roll renders only there, and BASIC
 * POKEs the attribute pairs to mark the song list. Neither surface
 * alone is what the hardware shows, so merge them the way
 * p8_frame merges the graphics planes: the image wins where it
 * holds a code, and its attribute pairs recolor the cells they
 * govern (including cells whose code came from the interpreter).
 *
 * Attribute pairs follow the CRTC "transparent" layout: a pair
 * (column, attr) takes effect at that column and holds until the
 * next pair; column 0x80 as the first pair is the row default.
 * An attr byte with bit 3 set is a color command (bits 7/6/5 =
 * G/R/B, bit 4 = semi-graphic mode); with bit 3 clear it is a
 * decoration command (bit 0 secret, bit 1 blink, bit 2 reverse,
 * bit 4 upper line, bit 5 under line). The running attribute
 * carries across rows. Color commands keep the decorations;
 * decoration commands keep the color and the semi-graphic flag
 * (the CRTC's color-attribute mode, which is what this program's
 * yellow 0xC8 headers and graphic 0x18/0x98 roll rows require).
 * Blink is not phased here (cells stay visible). Merged attr
 * bytes use the host's format: color number in bits 7..5, bit 4
 * = semi-graphic.
 */
#define TEXT_IMAGE 0xF3C8
static unsigned short merged_codes[25 * 80];
static unsigned char  merged_attrs[25 * 80];

static int sjis_lead(int b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }
static int sjis_trail(int b) { return (b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC); }

/* SJIS pair -> the interpreter's packed kanji code (JIS bytes),
 * the same encoding screen.c's put_cell produces. */
static void sjis_to_jis(int b1, int b2, int *j1, int *j2)
{
    int a = b1, c = b2;
    if (a > 0x9F) a -= 0x40;
    a = (a - 0x81) * 2 + 0x21;
    if (c > 0x7F) c -= 1;
    if (c >= 0x9E) { c -= 0x9E - 0x21; a += 1; }
    else c -= 0x1F;
    *j1 = a; *j2 = c;
}

static void merge_text(void)
{
    const unsigned short *tv = halp8_text_codes();
    const unsigned char  *ta = halp8_text_attrs();
    int run_color = 0xE0, run_graph = 0, run_secret = 0, run_reverse = 0;
    int x, y, j;
    for (y = 0; y < 25; y++) {
        int base = TEXT_IMAGE + y * 120;
        int slot[81];
        int attr_rest = 0, governed = 0, strong = 0;
        int row_active = 0;
        for (j = 0; j <= 80; j++) slot[j] = 0;
        for (j = 0; j < 120; j++) if (hal_peek(0, base + j)) row_active = 1;
        for (j = 0; j < 20; j++) {
            int column = hal_peek(0, base + 80 + j * 2);
            int attr = hal_peek(0, base + 80 + j * 2 + 1);
            if (j != 0 && column == 0) column = 0x80;
            if (j == 0 && column == 0x80) column = 0;
            if (column == 0x80 && !attr_rest) attr_rest = attr | 0x100;
            else if (column <= 80 && !slot[column]) slot[column] = attr | 0x100;
        }
        if (!slot[0] && attr_rest) {    /* default holds until the */
            for (j = 80; j; j--) {      /* first specified column */
                if (slot[j]) { int t = slot[j]; slot[j] = attr_rest; attr_rest = t; }
            }
            slot[0] = attr_rest;
        }
        for (x = 0; x < 80; x++) {
            int img = hal_peek(0, base + x);
            unsigned short code;
            unsigned char attr;
            if (slot[x]) {
                int a = slot[x] & 0xFF;
                governed = 1;
                if (a & 0x08) {         /* color command */
                    run_color = a & 0xE0;
                    run_graph = (a & 0x10) != 0;
                    strong = (a & 0xE0) != 0;
                } else {                /* decoration command: sets
                                         * the decorations, keeps the
                                         * color AND the graphic
                                         * flag (CRTC color-attr
                                         * mode) */
                    run_secret = (a & 0x01) != 0;
                    run_reverse = (a & 0x04) != 0;
                    strong = run_reverse;
                }
            }
            attr = (unsigned char)(run_color | (run_graph ? 0x10 : 0));
            if (run_secret) {
                merged_codes[y * 80 + x] = 0;
                merged_attrs[y * 80 + x] = attr;
            } else if (row_active && run_graph) {
                /* semi-graphic row (staff roll): every byte is a
                 * dot pattern, even 0x00 / 0x20 */
                merged_codes[y * 80 + x] = (unsigned short)img;
                merged_attrs[y * 80 + x] = attr;
            } else if (img && img != 0x20) {
                if (sjis_lead(img) && x + 1 < 80 &&
                    sjis_trail(hal_peek(0, base + x + 1))) {
                    int j1, j2;
                    sjis_to_jis(img, hal_peek(0, base + x + 1), &j1, &j2);
                    merged_codes[y * 80 + x] = (unsigned short)((j2 << 8) | (j1 - 0x20));
                    merged_attrs[y * 80 + x] = attr;
                    x++;
                    merged_codes[y * 80 + x] = (unsigned short)((j2 << 8) | ((j1 - 0x20) | 0x80));
                    merged_attrs[y * 80 + x] = attr;
                } else {
                    merged_codes[y * 80 + x] = (unsigned short)img;
                    merged_attrs[y * 80 + x] = attr;
                }
            } else {
                /* Interpreter cell. An image pair recolors it only
                 * when it actually changes something (a marker's
                 * color, graphic mode, reverse) - a pair that just
                 * restates the cell's own color leaves the
                 * interpreter's byte, decorations and all, alone. */
                merged_codes[y * 80 + x] = tv[y * 80 + x];
                merged_attrs[y * 80 + x] =
                    (governed && strong &&
                     (run_color != (ta[y * 80 + x] & 0xE0) ||
                      run_graph || run_reverse))
                        ? attr : ta[y * 80 + x];
            }
        }
    }
}

const unsigned short *p8_text_codes(P8Engine *e)
{
    static const unsigned short blank_codes[25 * 80];
    (void)e;
    /* Port 0x53 bit0 hides the text plane; checked before the
     * width-40 path below so both widths are covered. */
    if (!halp8_text_visible() || vessel_text_hidden()) return blank_codes;
    if (halp8_width40()) return halp8_text_codes();
    merge_text();
    return merged_codes;
}
const unsigned char  *p8_text_attrs(P8Engine *e)
{
    static const unsigned char blank_attrs[25 * 80];
    (void)e;
    if (!halp8_text_visible() || vessel_text_hidden()) return blank_attrs;
    if (halp8_width40()) return halp8_text_attrs();
    merge_text();
    return merged_attrs;
}
void p8_cursor(P8Engine *e, int *x, int *y, int *visible)
{
    (void)e;
    halp8_cursor(x, y, visible);
    /* Same effective text visibility as p8_text_codes/p8_text_attrs:
     * no cursor on a text plane that is not shown. Only the value
     * handed to the host is gated; the HAL's cursor state is left
     * alone, so the cursor comes back with the text. */
    if (!halp8_text_visible() || vessel_text_hidden()) *visible = 0;
}
