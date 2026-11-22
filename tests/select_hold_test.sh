#!/bin/sh
# MA2026 *SELECT cursor rate under a held key (docs/APPROXIMATIONS.md
# item 35). The *SELECT loop polls the key matrix with INP() and
# redraws a status field (PRINT + LINE ...,BF) on every pass; on the
# genuine machine (QUASI88, 4 MHz) a 1.0 s hold moves the song cursor
# 4 songs down (5 up), about 4.5 passes per second.
#
# The embed harness enters *SELECT with RETURN (cursor on song 1),
# holds cursor-down for 300 ms and dumps the guest memory. At the
# genuine rate that hold is 1-2 moves, so the cursor must rest on
# song 2 or 3. With only the flat per-statement charge the loop ran
# at 13 passes per second and the cursor reached song 5.
#
#   tests/select_hold_test.sh
#
# Needs build-embed/embed_test (./build-embed.sh) and the MA2026 disk
# (P8P_TEST_DISK, default ~/workspace/p88test/ma2026/MA2026A.D88).
# Real time, about 45 s. Exit status: 0 = PASS, 1 = FAIL.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
DISK=${P8P_TEST_DISK:-$HOME/workspace/p88test/ma2026/MA2026A.D88}
BIN=$ROOT/build-embed/embed_test
[ -x "$BIN" ] || { echo "FAIL: select_hold: $BIN missing (run ./build-embed.sh)"; exit 1; }
[ -f "$DISK" ] || { echo "FAIL: select_hold: no disk at $DISK"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# 40 s of attract, RETURN (enter *SELECT), 2 s to settle, cursor-down
# held 300 ms by the harness, 1 s for the last move to be drawn.
KEYS=$(printf '\\w%.0s' $(seq 40))
touch "$WORK/marker"
(cd "$WORK" && P8P_DUMPFULL=1 "$BIN" "$DISK" 1 \
   "${KEYS}\\E\\w\\w\\D\\w" > embed.log 2>&1)
# The harness writes full.bin into its own scratch dir (/tmp/p8pXXXXXX),
# not the working directory; take the dump this run produced.
DUMP=$(find /tmp -maxdepth 2 -name full.bin -newer "$WORK/marker" 2>/dev/null | head -1)
if [ -z "$DUMP" ] || [ ! -f "$DUMP" ]; then
  echo "FAIL: select_hold: no memory dump produced"
  tail -5 "$WORK/embed.log"
  exit 1
fi
cp "$DUMP" "$WORK/full.bin"

# The song cursor is the byte 0xC6 in the guest text image (base
# 0xF418, 120 bytes per row). menu.BAS places it at column XT*6+3 of
# row YT; the three list columns hold songs YT-1, YT+20 and YT+41.
python3 - "$WORK/full.bin" <<'EOF'
import sys
BASE, ROW, ROWS = 0xF418, 120, 25
mem = open(sys.argv[1], "rb").read()
img = mem[BASE:BASE + ROW * ROWS]
songs = []
for off, b in enumerate(img):
    if b != 0xC6:
        continue
    yt, x = divmod(off, ROW)
    if x < 3 or (x - 3) % 6:
        continue
    xt = (x - 3) // 6
    if xt > 2:
        continue
    songs.append(yt + (-1, 20, 41)[xt])
if len(songs) != 1:
    print("FAIL: select_hold: %d cursor bytes (0xC6) at list positions, want 1%s"
          % (len(songs), " (songs %s)" % songs if songs else ""))
    sys.exit(1)
n = songs[0]
if n not in (2, 3):
    print("FAIL: select_hold: cursor on song %d after a 300 ms hold, want 2 or 3"
          " (genuine rate: 4-5 moves per second)" % n)
    sys.exit(1)
print("PASS: select_hold: cursor on song %d after a 300 ms hold" % n)
EOF
