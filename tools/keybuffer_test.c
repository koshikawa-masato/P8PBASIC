/* keybuffer_test.c - unit checks for the embedded HAL's key buffer
 * (halp8.c kb_tick; docs/APPROXIMATIONS.md item 34): the filter
 * between the matrix the host feeds (halp8_set_matrix) and the one
 * the guest sees (hal_keymatrix, keyscan). It is asymmetric: a press
 * is visible at the first tick, a release only after 25 ms of raw
 * stability and 40 ms of visible press - except that a tick which
 * promotes a press also promotes every pending release that has had
 * its 40 ms (snapshot consistency; a second cell, SHIFT, is driven
 * for those cases). No guest runs: one cell
 * (the A key) is driven on a scripted timeline, the buffer and the
 * scanner are pumped through hal_idle, the visible matrix is sampled
 * with hal_keymatrix and the generated characters are read back with
 * hal_getkey. The buffer reads the wall clock, so the timeline is
 * real time (about 3 s in total). A flap whose steps were stretched
 * by the scheduler towards the release time is not the scripted
 * signal any more; such a run is repeated (a few times at most).
 * Exit 0 = all checks pass. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hal.h"
#include "halp8.h"

#define ROW 2               /* the A key: row 2, bit 1 */
#define BIT 1
#define MROW 8              /* SHIFT: row 8, bit 6 (the modifier cell) */
#define MBIT 6
#define FLAP_MS 3
#define STALL_MS 8          /* a flap step this long is a scheduler stall
                             * (it must stay under the 25 ms release time) */
#define PAIR_GAP_MS 15      /* the release half of a slow flap pair: longer
                             * than the former 10 ms release time, shorter
                             * than the 25 ms one */
#define PAIR_STALL_MS 22    /* a gap stretched this far is a scheduler stall
                             * (it must stay under the 25 ms release time) */
#define PAIRS 4
#define TRIES 5

static int fails;

static void result(const char *what, int ok, long got, long want)
{
    if (ok) printf("PASS %s\n", what);
    else { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}
static void chk(const char *what, long got, long want)
{
    result(what, got == want, got, want);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long last_feed;         /* when the cell was last fed */

static void feed(int down)
{
    last_feed = now_ms();
    halp8_set_matrix(ROW, (unsigned char)(down ? 0xFF & ~(1 << BIT) : 0xFF));
}
static int visible(void)
{
    return !(hal_keymatrix(ROW) & (1 << BIT));
}

/* A second cell (the modifier of the pull-through cases); it shares
 * no row with the A key, so each feed writes its own row. */
static void feed_cell(int row, int bit, int down)
{
    halp8_set_matrix(row, (unsigned char)(down ? 0xFF & ~(1 << bit) : 0xFF));
}
static int visible_cell(int row, int bit)
{
    return !(hal_keymatrix(row) & (1 << bit));
}

/* What the guest saw of the cell while the timeline ran. */
static int seen_down;                   /* the cell was visibly down */
static long long first_down, last_down; /* first / last sample that was down */
static int was_down;                    /* the previous sample was down */
static int drops;                       /* visible down -> up transitions */
static long long first_up;              /* first sample up after a down */

static void watch_reset(void)
{
    seen_down = was_down = drops = 0;
    first_down = last_down = first_up = 0;
}

/* Pump the buffer and the scanner for `ms` of real time, sampling
 * the visible cell. The character queue is left alone. */
static void run_ms(int ms)
{
    long long end = now_ms() + ms, t;
    do {
        hal_idle();
        if (hal_keyhit()) usleep(1000);     /* hal_idle only sleeps on an empty queue */
        t = now_ms();
        if (visible()) {
            if (!seen_down) first_down = t;
            seen_down = 1; last_down = t;
            was_down = 1;
        } else if (was_down) {
            if (!drops) first_up = t;
            drops++;
            was_down = 0;
        }
    } while (t < end);
}

/* Toggle the cell every FLAP_MS for `ms`, starting with a press and
 * ending at `end_down`. Returns the longest step actually taken. */
static long flap(int ms, int end_down)
{
    long long t0 = now_ms(), prev = t0, t;
    int down = 0;
    long worst = 0;
    while ((t = now_ms()) - t0 < ms || down != end_down) {
        if (t - prev > worst) worst = (long)(t - prev);
        prev = t;
        down = !down;
        feed(down);
        run_ms(FLAP_MS);
    }
    if (now_ms() - prev > worst) worst = (long)(now_ms() - prev);
    return worst;
}

/* Number of 'A's waiting in the queue (it is emptied). */
static int drain(void)
{
    int n = 0, k;
    while ((k = hal_getkey()) >= 0)
        if (k == 'A') n++;
    return n;
}

/* Everything up, both sides of the buffer settled, queue empty. */
static void settle(void)
{
    feed(0);
    run_ms(60);
    (void)drain();
    watch_reset();
}

int main(void)
{
    int n, try;
    long worst;
    long long t_release;

    halp8_session_reset();
    settle();
    chk("idle: cell up", visible(), 0);

    /* 1. a flap that ends up: the first press is visible at once and
     * the release + re-press pairs (3 ms each) never drop it; the
     * guest sees one press, released after the flap */
    for (try = 0; try < TRIES; try++) {
        settle();
        worst = flap(90, 0);
        if (worst < STALL_MS) break;
    }
    result("flap up: timeline held", worst < STALL_MS, worst, STALL_MS);
    chk("flap up: press visible", seen_down, 1);
    chk("flap up: never dropped during the flap", drops, 0);
    run_ms(60);
    chk("flap up: released after the flap", visible(), 0);
    chk("flap up: one release", drops, 1);
    chk("flap up: exactly one character", drain(), 1);

    /* 2. a flap that settles down: one press, held throughout */
    for (try = 0; try < TRIES; try++) {
        settle();
        worst = flap(60, 1);
        if (worst < STALL_MS) break;
    }
    result("flap down: timeline held", worst < STALL_MS, worst, STALL_MS);
    chk("flap down: press visible", seen_down, 1);
    chk("flap down: never dropped during the flap", drops, 0);
    run_ms(60);
    chk("flap down: still down", visible(), 1);
    chk("flap down: never dropped afterwards", drops, 0);
    chk("flap down: exactly one character", drain(), 1);

    /* 2b. slow flap pairs: a held key (long past its minimum press,
     * so only the stability wait guards it) whose raw cell goes up
     * for 15 ms and comes back, a few times over. The gap is longer
     * than the former 10 ms release time and shorter than the 25 ms
     * one: the visible press never drops and no second character is
     * generated (240 ms in all, short of the repeat delay) */
    for (try = 0; try < TRIES; try++) {
        int i;
        settle();
        feed(1);
        run_ms(60);
        worst = 0;
        for (i = 0; i < PAIRS; i++) {
            long gap;
            feed(0);
            run_ms(PAIR_GAP_MS);
            gap = (long)(now_ms() - last_feed);
            feed(1);
            if (gap > worst) worst = gap;
            run_ms(30);
        }
        if (worst < PAIR_STALL_MS) break;
    }
    result("slow pairs: timeline held", worst < PAIR_STALL_MS, worst, PAIR_STALL_MS);
    chk("slow pairs: press visible", seen_down, 1);
    chk("slow pairs: never dropped", drops, 0);
    chk("slow pairs: still down", visible(), 1);
    chk("slow pairs: exactly one character", drain(), 1);
    feed(0);
    run_ms(60);
    chk("slow pairs: released afterwards", visible(), 0);
    chk("slow pairs: one release", drops, 1);
    chk("slow pairs: nothing after the release", drain(), 0);

    /* 3. a quick tap (50 ms): seen, once, and for long enough */
    settle();
    feed(1);
    run_ms(50);
    feed(0);
    run_ms(90);
    chk("tap: press visible", seen_down, 1);
    chk("tap: released again", visible(), 0);
    result("tap: visible for at least 35 ms",
           last_down - first_down >= 35, (long)(last_down - first_down), 35);
    chk("tap: exactly one character", drain(), 1);

    /* a very short tap (5 ms) is promoted at once and stretched to
     * the minimum visible press: its release waits for the 40 ms */
    settle();
    feed(1);
    run_ms(5);
    feed(0);
    run_ms(90);
    chk("short tap: press visible", seen_down, 1);
    chk("short tap: released again", visible(), 0);
    result("short tap: visible for at least 35 ms",
           last_down - first_down >= 35, (long)(last_down - first_down), 35);
    chk("short tap: exactly one character", drain(), 1);

    /* 4. a normal hold (300 ms): visible at the first tick after the
     * feed, one character, no repeat yet; the release (long past the
     * minimum press) is promoted once it has been stable for 25 ms,
     * within a poll or so of that */
    settle();
    feed(1);
    hal_idle();
    chk("hold: visible at the first tick", visible(), 1);
    run_ms(300);
    chk("hold: visible", visible(), 1);
    chk("hold: never dropped", drops, 0);
    n = drain();
    chk("hold: exactly one character at 300 ms", n, 1);
    feed(0);
    t_release = last_feed;
    run_ms(80);
    chk("hold: released", visible(), 0);
    result("hold: release only after 25 ms",
           first_up - t_release >= 25, (long)(first_up - t_release), 25);
    result("hold: release within 55 ms",
           first_up - t_release <= 55, (long)(first_up - t_release), 55);
    chk("hold: nothing after the release", drain(), 0);

    /* 5. snapshot consistency (pull-through): a press never overtakes
     * a release that has had its minimum press. SHIFT is held, then
     * released and the A key pressed at once (well inside the 25 ms
     * release time, which the pull-through waives): the very first
     * tick shows A down AND SHIFT up, so the scanner never translates
     * A against the stale SHIFT */
    settle();
    feed_cell(MROW, MBIT, 1);
    run_ms(100);
    chk("pull: modifier held", visible_cell(MROW, MBIT), 1);
    (void)drain();
    watch_reset();
    feed_cell(MROW, MBIT, 0);
    feed(1);
    hal_idle();                         /* one tick */
    chk("pull: press visible at the first tick", visible(), 1);
    chk("pull: modifier release in the same tick", visible_cell(MROW, MBIT), 0);
    run_ms(60);
    chk("pull: unshifted character", drain(), 1);    /* 'A', not 'a' */

    /* the minimum press is not waived: a modifier released 5 ms after
     * it became visible stays visible when another press is promoted
     * (unless the scheduler stretched the 5 ms past the minimum) */
    settle();
    feed_cell(MROW, MBIT, 1);
    hal_idle();
    t_release = now_ms();               /* the modifier is visible from here */
    run_ms(5);
    feed_cell(MROW, MBIT, 0);
    feed(1);
    hal_idle();
    n = visible_cell(MROW, MBIT);
    result("pull: release short of the minimum press stays pending",
           n == 1 || now_ms() - t_release >= 40, n, 1);
    chk("pull: its press still visible at once", visible(), 1);
    run_ms(90);
    chk("pull: pending release promoted later", visible_cell(MROW, MBIT), 0);

    /* 6. no pull-through without a press promotion: with another cell
     * merely HELD (no new press edge anywhere), the flap of case 2 is
     * still absorbed */
    for (try = 0; try < TRIES; try++) {
        settle();
        feed_cell(MROW, MBIT, 1);
        run_ms(60);
        watch_reset();
        worst = flap(60, 1);
        if (worst < STALL_MS) break;
    }
    result("flap, other cell held: timeline held", worst < STALL_MS, worst, STALL_MS);
    chk("flap, other cell held: press visible", seen_down, 1);
    chk("flap, other cell held: never dropped", drops, 0);
    feed_cell(MROW, MBIT, 0);
    run_ms(60);
    chk("flap, other cell held: still down", visible(), 1);
    chk("flap, other cell held: never dropped afterwards", drops, 0);
    settle();

    /* session reset: a held cell does not survive on either side */
    feed(1);
    run_ms(60);
    chk("reset: held before", visible(), 1);
    halp8_session_reset();
    watch_reset();
    chk("reset: cell up at once", visible(), 0);
    run_ms(60);
    chk("reset: nothing promoted afterwards", seen_down, 0);
    chk("reset: queue empty", drain(), 0);

    if (fails) { printf("keybuffer: %d FAIL\n", fails); return 1; }
    printf("keybuffer ok\n");
    return 0;
}
