#!/bin/sh
# P8PBASIC regression suite. See tests/README.md for the tier design.
#
#   tests/run_regress.sh           build, run all tiers, report PASS/FAIL
#   tests/run_regress.sh --bless   regenerate the Tier B snapshot baselines
#   tests/run_regress.sh --pin-counters
#                                  record the embed audio counters
#                                  (tests/expected/embed55.counters)
#
# The game disk is located through P8P_TEST_DISK (default:
# ~/workspace/p88test/ma2026/MA2026A.D88). Without it the probe tiers
# cannot run (the probes BLOAD NEOMSC from the disk) and are skipped
# with a notice; the build tier still runs.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BLESS=0
[ "${1:-}" = "--bless" ] && BLESS=1
PIN=0
[ "${1:-}" = "--pin-counters" ] && PIN=1
DISK=${P8P_TEST_DISK:-$HOME/workspace/p88test/ma2026/MA2026A.D88}
FAIL=0
note() { echo "== $*"; }
bad() { echo "FAIL: $*"; FAIL=1; }

note "build"
./build-host.sh >/dev/null 2>&1 || { bad "build-host.sh"; echo "SUITE FAILED"; exit 1; }
./build-embed.sh >/dev/null 2>&1 || { bad "build-embed.sh"; echo "SUITE FAILED"; exit 1; }
# unit checks (text-screen merge; RAM window on the Z80 bus);
# they link the embed objects minus every test main.
EMBED_OBJS=$(ls build-embed/*.o | grep -v embed_test \
             | grep -v textmerge_test | grep -v window_test \
             | grep -v subsys_test | grep -v dramload_test | grep -v pcmb_ports_test \
             | grep -v kanjifont_test | grep -v gfxmirror_test \
             | grep -v restart_test | grep -v d88fix_test \
             | grep -v keyscan_test | grep -v keybuffer_test)
unit_test() { # name [args...]
  _t=$1; shift
  cc -O1 -std=gnu11 -Isrc -Ithird_party/z80 -c "tools/$_t.c" \
     -o "build-embed/$_t.o" 2>/dev/null \
    && c++ -o "build-embed/$_t" $EMBED_OBJS "build-embed/$_t.o" \
         -lm -lpthread 2>/dev/null \
    && "./build-embed/$_t" "$@" >/dev/null \
    || bad "$_t"
}
unit_test textmerge_test
unit_test window_test
unit_test kanjifont_test
unit_test pcmb_ports_test
unit_test gfxmirror_test
# Keyboard character generation from the key matrix (halp8.c keyscan;
# docs/APPROXIMATIONS.md item 32). Every key passes the key buffer
# in real time (20 ms debounce, 40 ms minimum press), so it takes
# roughly 10 s.
unit_test keyscan_test
# Key buffer between the host-fed matrix and the guest-visible one
# (halp8.c kb_tick; docs/APPROXIMATIONS.md item 34): chatter is
# swallowed, a settled press appears once, a quick tap stays visible
# long enough. Real time, about 1.5 s.
unit_test keybuffer_test
# Disk-layer boundaries on synthetic D88 images built by the test
# itself (review items C-2 / L-2), so this runs without the game
# disk: two bundled images, cut-off files, a broken track table, a
# zero-length sector, an oversized file and a directory entry named
# "../X", through the sector reader, the XDE natives, the subsystem
# commands, the file layer and the materializer. A request for an
# image or drive the file does not hold must FAIL.
unit_test d88fix_test
# Token conversion (review items T-4 / T-5), no disk needed.
# tools/p88conv_test.py checks the Python tools and the C converter
# (src/p88conv.c standalone driver) against exact text: MBF single /
# double constants, &O octal, labels passed through, STOP-trap drops
# (docs/APPROXIMATIONS.md item 26). Given the host interpreter (third
# argument) it also runs the converted programs and checks what they
# print: constant type / value / precedence (CONVEVAL) and the label
# rules (CONVLAB2). The label program it converts must
# then run in the host interpreter: GOSUB *TYPE sets A=1 and returns,
# IF A=1 THEN *OK reaches the PRINT.
CONVT=build-host/convtest
rm -rf "$CONVT"; mkdir -p "$CONVT"
cc -O1 -std=gnu11 -DP88CONV_TEST -Isrc src/p88conv.c -o build-host/p88conv 2>/dev/null \
  && python3 tools/p88conv_test.py build-host/p88conv "$CONVT" build-host/p8pbasic \
  || bad "p88conv_test"
if [ -f "$CONVT/CONVLAB.BAS" ]; then
  out=$(cd "$CONVT" && env -u P8P_DISK timeout 60 \
          "$ROOT/build-host/p8pbasic" CONVLAB.BAS 2>/dev/null) || bad "CONVLAB exit"
  printf '%s\n' "$out" | grep -q '^00|labrun 1$' \
    || bad "CONVLAB: converted label program did not run through its labels"
fi
echo "build ok"

if [ ! -f "$DISK" ]; then
  note "no game disk at $DISK -- probe/embed tiers skipped"
  [ $FAIL -eq 0 ] && echo "SUITE PASSED (build only)" || echo "SUITE FAILED"
  exit $FAIL
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK" /tmp/p8p??????' EXIT
cp tests/probes/*.BAS "$WORK/"
# With P8P_DISK set the host CLI materializes the disk's files into
# a scratch dir itself (src/materialize.c, shared with the embedded
# engine) and runs there, so the probes find the disk's own NEOMSC
# without any extracted plain files in $WORK.

# ---- subsystem write path (cmd 01H) against the real disk image:
# writes land in the in-memory image only; the file stays untouched.
unit_test subsys_test "$DISK"
echo "subsys ok"

# ---- FFDE service 1 (vessel_dram_load) must fill the OPNA's
# ADPCM-B DRAM with the disk's sectors and leave ADPCM-A untouched.
# (No unconditional "ok" echo: unit_test records failures itself.)
unit_test dramload_test "$DISK"

run_probe() { # name [env assignments...]
  _n=$1; shift
  (cd "$WORK" && env P8P_DISK="$DISK" "$@" timeout 60 \
     "$ROOT/build-host/p8pbasic" "$_n.BAS" 2>/dev/null)
}

# ---- Tier A: semantic assertions (values verified against the genuine
# machine / the NEOMSC contract, not just "what the code prints today").
note "Tier A assertions"
out=$(run_probe PROBE2) || bad "PROBE2 exit"
printf '%s\n' "$out" | grep -q '^00| 0  202$' || bad "PROBE2: (AA0F/AA10) != 0,202"
printf '%s\n' "$out" | grep -q '^01| 0  192$' || bad "PROBE2: (AA13/AA14) != 0,192"
out=$(run_probe PROBE18) || bad "PROBE18 exit"
printf '%s\n' "$out" | grep -q '60848' || bad "PROBE18: song-DB pointer 60848 missing"
printf '%s\n' "$out" | grep -q '9081' || bad "PROBE18: song-DB pointer 9081 missing"
out=$(run_probe PROBE19) || bad "PROBE19 exit"
printf '%s\n' "$out" | grep -q 'done8' || bad "PROBE19: XDD 8 did not complete"
out=$(run_probe PROBE42) || bad "PROBE42 exit"
printf '%s\n' "$out" | grep -q 'waited' || bad "PROBE42: playback wait loop broken"
out=$(run_probe PROBE44) || bad "PROBE44 exit"
printf '%s\n' "$out" | grep -q 'ok44' || bad "PROBE44: CG load/draw path broken"
# PROBE: *label GOTO (upstream Ver.261119 label support; on the
# pre-merge base this errored at line 10, which the old Tier B
# baseline honestly captured)
out=$(run_probe PROBE) || bad "PROBE exit"
printf '%s\n' "$out" | grep -q 'oklab' || bad "PROBE: label GOTO *FOO did not reach the label"
# PROBE53: the FFDE service-3 staff roll renders into the guest text
# image at 0xF3C8 as semi-graphic bitmap codes (font at 0xCA00,
# 8 bytes per character; the scroll shifts the dot plane). With the
# probe's fixed staff data the image ends with exactly 384 nonzero
# bytes. Control runs: no service-3 calls -> all zero; different
# staff text -> a different image (348). Before the ROM string
# evaluator (0x11D3) was emulated, the draw path read a stale
# numeric descriptor and produced a fixed 1728-byte pattern
# regardless of the text. The embed tier below checks that the
# engine's merged text surface displays the image (staff roll
# rows, attribute-pair recoloring) alongside interpreter text.
out=$(run_probe PROBE53) || bad "PROBE53 exit"
printf '%s\n' "$out" | grep -q 'rolldone' || bad "PROBE53: staff roll did not complete"
img=$(printf '%s\n' "$out" | grep -o 'img *[0-9]*' | grep -o '[0-9]*$')
if [ "$img" != "384" ]; then
  bad "PROBE53: text image count ${img:-none} != 384 (staff roll render changed)"
fi
# PROBE54 / PROBE55: the NEOMSC-only traps must stay disarmed when no
# NEOMSC was BLOADed (docs/APPROXIMATIONS.md item 25). Neither probe
# loads NEOMSC; each POKEs a tiny routine, BSAVEs it and runs it with
# BLOAD ,R (CALL does not reach the vessel in the host HAL).
# PROBE54: a counter routine at 0xAB00, run twice -> the cell reads 2
# (with the "PMD not loaded" RET-only range armed it stayed 0).
# PROBE55: a routine at 0xA794 that stores DE=0x1234 and bumps a
# counter -> 1, 0x34, 0x12 (with the argument-fetch trap armed the
# routine was replaced by a DE rewrite + RET and all three stayed 0).
out=$(run_probe PROBE54) || bad "PROBE54 exit"
printf '%s\n' "$out" | grep -q '^00|ab00 ran 2$' || bad "PROBE54: code at 0xAB00 did not run without NEOMSC"
out=$(run_probe PROBE55) || bad "PROBE55 exit"
printf '%s\n' "$out" | grep -q '^00|a794 ran 1  52  18$' || bad "PROBE55: code at 0xA794 did not run as written without NEOMSC"
# PROBE56: entry registers of the statement relays, seen by a custom
# handler (no NEOMSC): a recorder at 0x9810 stores A, BC, HL and the
# byte at (HL); a JP to it is planted at each EEA vector in turn.
# Printed as A B C H L (HL). Per docs/NEOMSC-CONTRACT.md: BC = the
# vector address (0xEEA7 / 0xEEA1 / 0xEEA4), A = the command letter
# ('R' = 82) for XDF and the first argument's token byte for XDD/XDE
# (digit tokens 5 -> 0x16, 3 -> 0x14), HL = the address of that byte
# in the relay's stream (0x8F03). For XDE only A and BC are asserted:
# the genuine HL there is 0xE69C, a deviation the vessel keeps on
# purpose (see stmt_ext); the Tier B snapshot still records it.
out=$(run_probe PROBE56) || bad "PROBE56 exit"
printf '%s\n' "$out" | grep -q '^00|xdf 82  238  167  143  3  82$' || bad "PROBE56: XDF entry registers != A='R' BC=EEA7 HL->'R'"
printf '%s\n' "$out" | grep -q '^01|xdd 22  238  161  143  3  22$' || bad "PROBE56: XDD entry registers != A=token BC=EEA1 HL->token"
printf '%s\n' "$out" | grep -q '^02|xde 20  238  164 ' || bad "PROBE56: XDE entry registers != A=token BC=EEA4"
# PROBE57: a notified load that overlaps the NEOMSC body without being
# the NEOMSC load disarms the NEOMSC traps: NEOMSC is BLOADed (traps
# armed), then PROBE55's routine is BLOADed over 0xA794 and must run
# as written (with the detection left armed all three cells stay 0).
out=$(run_probe PROBE57) || bad "PROBE57 exit"
printf '%s\n' "$out" | grep -q '^00|a794 ran 1  52  18$' || bad "PROBE57: overlapping load did not disarm the 0xA794 trap"
[ $FAIL -eq 0 ] && echo "Tier A ok"

note "Tier A audio (standalone songs must be loud from the start)"
for p in PROBE5 PROBE51 PROBE52; do
  (cd "$WORK" && env P8P_DISK="$DISK" P8PWAV="$WORK/$p.wav" timeout 120 \
     "$ROOT/build-host/p8pbasic" "$p.BAS" >/dev/null 2>&1) \
    || { bad "$p exit"; continue; }
  python3 tests/wavprofile.py probe "$WORK/$p.wav" >/dev/null \
    || bad "$p audio profile"
done
[ $FAIL -eq 0 ] && echo "Tier A audio ok"

# ---- Tier B: characterization snapshots of every probe's final screen.
# A diff means behaviour changed; a human decides whether the change is
# the intended one and re-blesses. These are NOT correctness proofs.
note "Tier B snapshots"
for f in tests/probes/*.BAS; do
  n=$(basename "$f" .BAS)
  out=$(run_probe "$n")
  rc=$?
  [ $rc -ne 0 ] && { bad "$n exit rc=$rc"; continue; }
  if [ $BLESS -eq 1 ]; then
    printf '%s\n' "$out" > "tests/expected/$n.txt"
  else
    printf '%s\n' "$out" > "$WORK/$n.out"
    if ! diff -q "tests/expected/$n.txt" "$WORK/$n.out" >/dev/null 2>&1; then
      bad "$n screen differs from baseline (diff below)"
      diff -u "tests/expected/$n.txt" "$WORK/$n.out" | head -20
    fi
  fi
done
[ $BLESS -eq 1 ] && echo "baselines regenerated"
[ $FAIL -eq 0 ] && echo "Tier B ok"

# ---- Embed tier: the whole MA2026 flow in the embedded engine.
note "embed: title CG (40 s run)"
rm -rf /tmp/p8p??????
(cd /tmp && P8P_VRAMDUMP=1 "$ROOT/build-embed/embed_test" "$DISK" 40 \
   > "$WORK/embed40.log" 2>&1)
D=$(ls -td /tmp/p8p* 2>/dev/null | head -1)
grep -q 'running=1' "$WORK/embed40.log" || bad "embed40: engine not running at exit"
DR=$(sed -n 's/.*dropped=\([0-9]*\).*/\1/p' "$WORK/embed40.log")
[ -n "$DR" ] && [ "$DR" -le 20000 ] || bad "embed40: dropped=$DR > 20000"
if [ -n "$D" ] && [ -f "$D/vram0.bin" ]; then
  python3 - "$D" <<'EOF' || bad "embed40: VRAM plane counts changed"
import sys
want = [7054, 7149, 7876]
got = []
for b in range(3):
    data = open("%s/vram%d.bin" % (sys.argv[1], b), "rb").read()
    got.append(sum(1 for x in data if x))
if got != want:
    print("VRAM counts", got, "want", want)
    sys.exit(1)
EOF
else
  bad "embed40: no VRAM dump produced"
fi
[ $FAIL -eq 0 ] && echo "embed CG ok"

note "embed: audio phases (55 s run)"
rm -rf /tmp/p8p??????
(cd /tmp && P8P_COUNTERS_AT=50 "$ROOT/build-embed/embed_test" "$DISK" 55 \
   > "$WORK/embed55.log" 2>&1)
D=$(ls -td /tmp/p8p* 2>/dev/null | head -1)
DR=$(sed -n 's/.*dropped=\([0-9]*\).*/\1/p' "$WORK/embed55.log")
[ -n "$DR" ] && [ "$DR" -le 20000 ] || bad "embed55: dropped=$DR > 20000"
if [ -n "$D" ] && [ -f "$D/embed_audio.wav" ]; then
  python3 tests/wavprofile.py embed "$D/embed_audio.wav" \
    || bad "embed55: audio phase order differs from the genuine machine"
else
  bad "embed55: no audio captured"
fi
[ $FAIL -eq 0 ] && echo "embed audio ok"

# ---- Embed audio CONTENT (review item C-1). The peak / phase checks
# above pass with the music at twice its tempo (that bug was caught
# by ear), so the same 55 s run also freezes the vessel's diagnostic
# counters at emulated second 50 (P8P_COUNTERS_AT; emulated time, so
# the values do not depend on host scheduling) and they must equal
# the pinned values in tests/expected/embed55.counters exactly. What
# each line pins is written in that file's header (below).
# Pin them once from a build known to be correct:
#   tests/run_regress.sh --pin-counters
# and review the diff like a baseline; --bless does not touch them.
CNT=tests/expected/embed55.counters
grep '^counter ' "$WORK/embed55.log" > "$WORK/embed55.counters"
if ! grep -q '^counter tb_irqs=' "$WORK/embed55.counters"; then
  bad "embed55: no counter snapshot (emulated second 50 not reached in 55 s)"
elif [ $PIN -eq 1 ]; then
  {
    cat <<'EOF'
# Embed audio tier, 55 s MA2026 run: vessel diagnostic counters at
# emulated second 50 (tests/run_regress.sh, review item C-1).
# Written by tests/run_regress.sh --pin-counters; exact match required.
#   tb_irqs        Timer B interrupt vectors queued = sequencer ticks:
#                  the tempo (a 2x-tempo build doubles it)
#   vrtc_irqs      VRTC interrupt vectors queued: the 60 Hz cadence and
#                  the VSYNC mask / priority handling
#   b0_ssg         bank-0 writes 00-0F: the SSG parts
#   b0_rhythm      bank-0 writes 10-1F: the rhythm (ADPCM-A) part
#   b0_mode        bank-0 writes 20-2F: timer control, acknowledges,
#                  key on/off
#   b0_fm          bank-0 writes 30-FF: FM channels 1-3 (tone, volume,
#                  pitch)
#   b0_keyon       bank-0 writes to 28: FM notes started / stopped
#   b0_tb          bank-0 writes to 26: tempo settings
#   b1_adpcm       bank-1 writes 00-2F: the ADPCM-B (PCM) part's
#                  registers, through PMD's AC/AD port pair
#   b1_fm          bank-1 writes 30-FF: FM channels 4-6
#   adpcm_b_reads  sample bytes the chip fetched from the ADPCM-B DRAM:
#                  PCM actually played from the loaded block
EOF
    cat "$WORK/embed55.counters"
  } > "$CNT"
  echo "counters pinned:"; cat "$WORK/embed55.counters"
elif [ -f "$CNT" ]; then
  if ! grep -v '^#' "$CNT" | diff -u - "$WORK/embed55.counters"; then
    bad "embed55: diagnostic counters differ from $CNT (diff above)"
  else
    echo "embed counters ok"
  fi
else
  bad "embed55: $CNT is missing -- the counter baseline ships with the suite and must be present (re-pin with: tests/run_regress.sh --pin-counters)"
fi

# The merged text surface: hold RETURN during attract to enter
# *SELECT (SCREEN 0,2, list + help printed by the interpreter),
# then check the list survived the merge (no image shadowing) and
# that the guest image's attribute pairs recolored merged cells
# (the pair build paints the two song columns green/cyan: merged
# attr bytes a0 appear, the interpreter's own plane has none).
note "embed: text merge (select screen)"
KEYS=$(printf '\\w%.0s' $(seq 50))
(cd /tmp && "$ROOT/build-embed/embed_test" "$DISK" 5 \
   "${KEYS}\\t\\w\\w\\w\\T\\w" > "$WORK/embedsel.log" 2>&1)
sed -n '/----- SCREEN -----/,/----- ATTRS (raw)/p' "$WORK/embedsel.log" \
  | grep -q 'MEGAFEPPS MUSIC ARENA 2026' || bad "embedsel: header missing from merged text"
sed -n '/----- SCREEN -----/,/----- ATTRS (raw)/p' "$WORK/embedsel.log" \
  | grep -q 'Move\[ASDW' || bad "embedsel: song list / help missing from merged text"
sed -n '/----- ATTRS (merged) -----/,$p' "$WORK/embedsel.log" \
  | grep -q 'a0a0' || bad "embedsel: no pair-driven recoloring in merged attrs"
[ $FAIL -eq 0 ] && echo "embed text ok"

# *SELECT cursor rate under a held key: INP / PRINT / LINE charge
# their modelled work, so the INP-polling loop iterates at the
# genuine rate (docs/APPROXIMATIONS.md item 35). The assertion reads
# MA2026's own cursor layout, so it runs for that disk only.
note "embed: select hold rate (45 s run)"
case "$DISK" in
  */MA2026A.D88) sh tests/select_hold_test.sh || bad "select_hold" ;;
  *) echo "select hold skipped: the test disk is not MA2026A.D88" ;;
esac

# In-process restart: the app creates a fresh engine per game launch
# in one process, so a second create/boot/start after stop/destroy
# must behave like the first (docs/APPROXIMATIONS.md item 22). Both
# boots sample the frame at the same point of the title sequence:
# the hashes must be equal, boot1's reference frame must be nonblank
# (two identical blank frames must not pass), and boot2 must be
# alive at the end and audible (restart_test's own exit status
# covers "running at t=3").
note "embed: in-process restart (2 x 45 s)"
if cc -O1 -std=gnu11 -Isrc -Ithird_party/z80 -c tools/restart_test.c \
      -o build-embed/restart_test.o 2>/dev/null \
   && c++ -o build-embed/restart_test $EMBED_OBJS build-embed/restart_test.o \
        -lm -lpthread 2>/dev/null; then
  (cd /tmp && "$ROOT/build-embed/restart_test" "$DISK" 45 \
     > "$WORK/restart.log" 2>&1) || bad "restart: boot2 dead or silent"
  H1=$(sed -n 's/^boot1: frame_hash=\([0-9a-f]*\).*/\1/p' "$WORK/restart.log")
  H2=$(sed -n 's/^boot2: frame_hash=\([0-9a-f]*\).*/\1/p' "$WORK/restart.log")
  A2=$(sed -n 's/^boot2: end .*audio_frames=\([0-9]*\).*/\1/p' "$WORK/restart.log")
  N1=$(sed -n 's/^boot1: frame_hash=.* nonzero_pixels=\([0-9]*\).*/\1/p' "$WORK/restart.log")
  R2=$(sed -n 's/^boot2: end running=\([0-9]*\).*/\1/p' "$WORK/restart.log")
  [ -n "$H1" ] && [ "$H1" = "$H2" ] \
    || bad "restart: frame hash boot1=${H1:-none} boot2=${H2:-none}"
  [ -n "$N1" ] && [ "$N1" -gt 10000 ] \
    || bad "restart: boot1 reference frame blank (nonzero_pixels=${N1:-none})"
  [ "$R2" = 1 ] \
    || bad "restart: boot2 end running=${R2:-none}"
  [ -n "$A2" ] && [ "$A2" -gt 0 ] \
    || bad "restart: boot2 audio_frames=${A2:-none}"
else
  bad "restart_test build"
fi
[ $FAIL -eq 0 ] && echo "embed restart ok"

echo
[ $FAIL -eq 0 ] && echo "SUITE PASSED" || echo "SUITE FAILED"
exit $FAIL
