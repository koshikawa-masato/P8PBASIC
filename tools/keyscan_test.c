/* keyscan_test.c - unit checks for the embedded HAL's keyboard
 * character generation (halp8.c keyscan; docs/APPROXIMATIONS.md
 * item 32). No guest runs: matrix cells are set through
 * halp8_set_matrix (active-low: pressed = bit cleared), the scanner
 * is pumped through hal_idle / hal_getkey, and the generated codes
 * are read back with hal_getkey. The matrix reaches the scanner
 * through the key buffer (kb_tick; item 34: instant press, release
 * after 10 ms of stability and a 40 ms minimum visible press), so every wait below is real time and long
 * enough for a press or a release to pass it (roughly 10 s in total,
 * the auto-repeat checks included). Exit 0 = all checks pass. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "hal.h"
#include "halp8.h"
#include "basic.h"          /* running: STOP / CTRL+C break only while set */

static int fails;
static unsigned char mx[16];

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

static void press(int row, int bit)
{
    mx[row] &= (unsigned char)~(1 << bit);
    halp8_set_matrix(row, mx[row]);
}
static void release(int row, int bit)
{
    mx[row] |= (unsigned char)(1 << bit);
    halp8_set_matrix(row, mx[row]);
}

/* Pump the scanner for 40 ms (ample for the key buffer: a press is
 * promoted at the first tick); first code produced or -1. */
static int next_key(void)
{
    long long end = now_ms() + 40;
    int k;
    do {
        hal_idle();
        k = hal_getkey();
        if (k >= 0) return k;
    } while (now_ms() < end);
    return -1;
}

/* Let the scanner observe the current matrix (a release followed
 * by a re-press between two scans is invisible to it otherwise).
 * 70 ms: a key released the moment it became visible stays visible
 * for the key buffer's 40 ms minimum press. */
static void pump(void)
{
    long long end = now_ms() + 70;
    do hal_idle(); while (now_ms() < end);
    while (hal_getkey() >= 0) ;
}

/* Release everything and throw away what is left in the queue. */
static void settle(void)
{
    int i;
    for (i = 0; i < 16; i++) { mx[i] = 0xFF; halp8_set_matrix(i, 0xFF); }
    while (next_key() >= 0) ;
}

/* Press one cell: it must produce exactly `want` (-1 = nothing). */
static void key_expect(const char *what, int row, int bit, int want)
{
    int k, extra;
    press(row, bit);
    k = next_key();
    extra = next_key();
    release(row, bit);
    result(what, k == want && extra == -1, k == want ? extra : k, k == want ? -1 : want);
    while (next_key() >= 0) ;
}

/* Press and release a cell that must not produce a character. */
static void tap_silent(const char *what, int row, int bit)
{
    key_expect(what, row, bit, -1);
}

/* Count the `code`s arriving within the next `ms` of real time. */
static int count_for(int ms, int code)
{
    long long end = now_ms() + ms;
    int n = 0, k;
    while (now_ms() < end) {
        hal_idle();
        while ((k = hal_getkey()) >= 0)
            if (k == code) n++;
    }
    return n;
}

int main(void)
{
    int n;

    halp8_session_reset();
    memset(mx, 0xFF, sizeof(mx));
    settle();
    (void)hal_breakflag();

    /* letters: SHIFT gives lowercase, CAPS inverts that */
    key_expect("A plain", 2, 1, 'A');
    press(8, 6);
    chk("SHIFT alone gives nothing", next_key(), -1);
    key_expect("SHIFT+B", 2, 2, 'b');
    release(8, 6);
    press(10, 7);                       /* CAPS lock: cell held down */
    chk("CAPS itself gives nothing", next_key(), -1);
    key_expect("CAPS on: C", 2, 3, 'c');
    press(8, 6);
    key_expect("CAPS on: SHIFT+C", 2, 3, 'C');
    release(8, 6);
    release(10, 7);
    pump();
    key_expect("CAPS off: C", 2, 3, 'C');

    /* digits and punctuation */
    key_expect("1", 6, 1, '1');
    press(8, 6);
    key_expect("SHIFT+1", 6, 1, '!');
    key_expect("SHIFT+0 gives nothing", 6, 0, -1);
    key_expect("SHIFT+- is =", 5, 7, '=');
    key_expect("SHIFT+_key is _", 7, 7, '_');
    release(8, 6);
    key_expect("_key unshifted gives nothing", 7, 7, -1);
    key_expect("YEN", 5, 4, 0x5C);
    key_expect("main 5", 6, 5, '5');
    key_expect("keypad 5", 0, 5, '5');
    press(8, 6);
    key_expect("SHIFT+keypad 5", 0, 5, '5');
    release(8, 6);
    key_expect("keypad -", 10, 5, '-');

    /* control keys */
    key_expect("RETURN", 1, 7, 13);
    key_expect("SPACE", 9, 6, 32);
    key_expect("ESC", 9, 7, 27);
    key_expect("TAB", 10, 0, 9);
    key_expect("RIGHT", 8, 2, 0x1C);
    key_expect("LEFT", 10, 2, 0x1D);
    key_expect("UP", 8, 1, 0x1E);
    key_expect("DOWN", 10, 1, 0x1F);
    key_expect("HOME", 8, 0, 0x0B);
    key_expect("DEL", 8, 3, 0x7F);
    key_expect("F1", 9, 1, KC_F1);
    key_expect("F5", 9, 5, KC_F1 + 4);
    key_expect("HELP", 10, 3, KC_HELP);
    key_expect("ROLL DOWN", 11, 1, KC_ROLLDN);
    press(8, 6);
    key_expect("SHIFT+F1", 9, 1, KC_SF1);
    key_expect("SHIFT+HOME", 8, 0, KC_SHOME);
    key_expect("SHIFT+INSDEL", 8, 3, 0x12);
    release(8, 6);

    /* break: STOP and CTRL+C set the flag and produce no character,
     * but only while a program runs; at the direct-mode prompt they
     * are inert, as on the genuine machine */
    tap_silent("STOP at prompt gives no char", 9, 0);
    chk("STOP at prompt sets no break", hal_breakflag(), 0);
    running = 1;
    chk("break flag idle", hal_breakflag(), 0);
    tap_silent("STOP gives no char", 9, 0);
    chk("STOP sets break flag", hal_breakflag(), 1);
    chk("break flag clears on read", hal_breakflag(), 0);
    press(8, 7);
    key_expect("CTRL+C gives no char", 2, 3, -1);
    chk("CTRL+C sets break flag", hal_breakflag(), 1);
    key_expect("CTRL+A", 2, 1, 1);
    key_expect("CTRL+1 gives nothing", 6, 1, -1);
    release(8, 7);
    chk("CTRL+A / CTRL+1 leave break flag", hal_breakflag(), 0);
    running = 0;

    /* kana lock */
    press(8, 5);                        /* kana lock: cell held down */
    chk("KANA itself gives nothing", next_key(), -1);
    key_expect("kana: A", 2, 1, 0xC1);
    key_expect("kana: 3", 6, 3, 0xB1);
    key_expect("kana: @", 2, 0, 0xDE);
    press(8, 6);
    key_expect("kana: SHIFT+3", 6, 3, 0xA7);
    key_expect("kana: SHIFT+A", 2, 1, 0xC1);
    release(8, 6);
    key_expect("kana: keypad 5", 0, 5, '5');
    release(8, 5);
    pump();
    key_expect("kana off: A", 2, 1, 'A');

    /* a lock held in the same snapshot as a letter governs it */
    press(8, 5); press(2, 1);
    chk("KANA+A same scan", next_key(), 0xC1);
    release(2, 1); release(8, 5);
    pump();
    key_expect("kana off after release: A", 2, 1, 'A');
    press(10, 7); press(2, 1);
    chk("CAPS+A same scan", next_key(), 'a');
    release(2, 1); release(10, 7);
    pump();
    key_expect("caps off after release: A", 2, 1, 'A');

    /* GRPH held: nothing is generated */
    press(8, 4);
    key_expect("GRPH+A gives nothing", 2, 1, -1);
    release(8, 4);
    key_expect("GRPH released: A", 2, 1, 'A');

    /* auto-repeat: 500 ms delay, then every 55 ms */
    settle();
    press(2, 1);
    chk("repeat: first A", next_key(), 'A');
    n = count_for(300, 'A');
    chk("repeat: none in the first 300 ms", n, 0);
    n = count_for(450, 'A');
    result("repeat: at least 2 by 750 ms", n >= 2, n, 2);
    release(2, 1);
    (void)count_for(40, 'A');           /* let the scanner see the release */
    n = count_for(200, 'A');
    chk("repeat: stops on release", n, 0);

    /* releasing the target stops the repeat even with another key held;
     * a second press takes the repeat over */
    press(2, 1);
    chk("repeat2: first A", next_key(), 'A');
    press(2, 2);
    chk("repeat2: then B", next_key(), 'B');
    n = count_for(700, 'A');
    chk("repeat2: A no longer repeats", n, 0);
    release(2, 2);
    (void)count_for(40, 'B');
    n = count_for(200, 'B') + count_for(1, 'A');
    chk("repeat2: B released, A held: silence", n, 0);
    settle();

    /* GRPH pressed while a repeat is running halts the repeats */
    press(2, 1);
    chk("repeat3: first A", next_key(), 'A');
    n = count_for(700, 'A');
    result("repeat3: repeats before GRPH", n >= 1, n, 1);
    press(8, 4);
    (void)count_for(40, 'A');           /* let GRPH pass the key buffer */
    n = count_for(250, 'A');
    chk("repeat3: GRPH halts repeats", n, 0);
    release(8, 4);
    release(2, 1);
    settle();

    /* session reset drops the lock states */
    press(10, 7);
    chk("CAPS held before reset gives nothing", next_key(), -1);
    halp8_session_reset();
    memset(mx, 0xFF, sizeof(mx));
    key_expect("after reset: A is uppercase", 2, 1, 'A');

    if (fails) { printf("keyscan: %d FAIL\n", fails); return 1; }
    printf("keyscan ok\n");
    return 0;
}
