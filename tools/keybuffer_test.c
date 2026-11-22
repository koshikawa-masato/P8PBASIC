/* keybuffer_test.c - unit checks for the embedded HAL's key buffer
 * (halp8.c kb_tick; docs/APPROXIMATIONS.md item 34): the debounce
 * between the matrix the host feeds (halp8_set_matrix) and the one
 * the guest sees (hal_keymatrix, keyscan). No guest runs: one cell
 * (the A key) is driven on a scripted timeline, the buffer and the
 * scanner are pumped through hal_idle, the visible matrix is sampled
 * with hal_keymatrix and the generated characters are read back with
 * hal_getkey. The buffer reads the wall clock, so the timeline is
 * real time (about 1.5 s in total). A flap whose steps were stretched
 * by the scheduler past the debounce time is not the scripted signal
 * any more; such a run is repeated (a few times at most).
 * Exit 0 = all checks pass. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hal.h"
#include "halp8.h"

#define ROW 2               /* the A key: row 2, bit 1 */
#define BIT 1
#define FLAP_MS 3
#define STALL_MS 15         /* a flap step this long is a scheduler stall */
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

/* What the guest saw of the cell while the timeline ran. */
static int seen_down;                   /* the cell was visibly down */
static long long first_down, last_down; /* first / last sample that was down */

static void watch_reset(void)
{
    seen_down = 0; first_down = last_down = 0;
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
    long long t_settled, t_press;

    halp8_session_reset();
    settle();
    chk("idle: cell up", visible(), 0);

    /* 1. chatter that ends up: the guest never sees the cell down */
    for (try = 0; try < TRIES; try++) {
        settle();
        worst = flap(90, 0);
        if (worst < STALL_MS) break;
    }
    result("chatter: timeline held", worst < STALL_MS, worst, STALL_MS);
    run_ms(60);
    chk("chatter: never visible", seen_down, 0);
    chk("chatter: queue empty", drain(), 0);

    /* 2. chatter that settles down: one press, after the settling */
    for (try = 0; try < TRIES; try++) {
        settle();
        worst = flap(60, 1);
        t_settled = last_feed;          /* flap's last feed: the final press */
        if (worst < STALL_MS && !seen_down) break;
    }
    result("settle: timeline held", worst < STALL_MS, worst, STALL_MS);
    chk("settle: not visible during the flap", seen_down, 0);
    chk("settle: no character during the flap", hal_keyhit(), 0);
    run_ms(60);
    chk("settle: press visible once settled", seen_down, 1);
    result("settle: only after the debounce time",
           first_down - t_settled >= 20, (long)(first_down - t_settled), 20);
    chk("settle: still down", visible(), 1);
    chk("settle: exactly one character", drain(), 1);

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

    /* a tap just past the debounce time (25 ms) is stretched to the
     * minimum visible press */
    settle();
    feed(1);
    run_ms(25);
    feed(0);
    run_ms(90);
    chk("short tap: press visible", seen_down, 1);
    chk("short tap: released again", visible(), 0);
    result("short tap: visible for at least 35 ms",
           last_down - first_down >= 35, (long)(last_down - first_down), 35);
    chk("short tap: exactly one character", drain(), 1);

    /* 4. a normal hold (300 ms): one character, no repeat yet */
    settle();
    t_press = now_ms();
    feed(1);
    run_ms(300);
    chk("hold: visible", visible(), 1);
    result("hold: visible within 40 ms",
           first_down - t_press <= 40, (long)(first_down - t_press), 40);
    n = drain();
    chk("hold: exactly one character at 300 ms", n, 1);
    feed(0);
    run_ms(80);
    chk("hold: released", visible(), 0);
    chk("hold: nothing after the release", drain(), 0);

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
