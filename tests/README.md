# P8PBASIC regression suite

Run from the repository root:

    tests/run_regress.sh

Regenerate the Tier B baselines after an intended behaviour change:

    tests/run_regress.sh --bless

The suite needs the MA2026 game disk. Its path comes from
`P8P_TEST_DISK` (default `~/workspace/p88test/ma2026/MA2026A.D88`).
Without the disk only the build tier runs. Note the split in how the
disk is consumed: the vessel's XDE sector services read the D88
directly (`P8P_DISK`), but the host CLI's BASIC file layer
(`src/fileio.c`) reads plain host files, so the runner extracts the
disk's own NEOMSC (with `tools/p88_extract.py`, byte-identical) into
the probe working directory for `BLOAD "NEOMSC",R`. The embedded
engine has no such split: `src/p88disk.c` serves BASIC files from
the image itself.

## Tiers

- **Build** — `build-host.sh` and `build-embed.sh` must both succeed.
  The unit checks run here too, including the token conversion test
  (`tools/p88conv_test.py`, no disk needed): the Python tools and the
  C converter must produce the same exact text for MBF single/double
  constants, `&O` octal, labels (passed through) and the STOP-trap
  drops, and the converted label program must run in the host
  interpreter. The converted constants and label fixtures are also
  run in the host interpreter and their printed results checked
  (type, value and precedence of constants; label separators, case
  folding, dotted names), and the last line of each fixture is a
  plain statement (final-line framing). `tools/d88fix_test.c` also
  runs here, without the game disk: it builds synthetic D88 images
  (two bundled images, cut-off files, a broken track table, a
  zero-length sector, an oversized file, a directory entry named
  `../X`) and checks that the sector reader, the XDE natives, the
  disk subsystem commands, the file layer and the materializer fail
  on what they cannot read instead of substituting other data
  (docs/APPROXIMATIONS.md item 28).
- **Tier A (assertions)** — probe outcomes whose correct values are
  known from the genuine machine and the NEOMSC contract
  (docs/NEOMSC-CONTRACT.md): the FFDF data-slot pointers, the song-DB
  pointers, XDD 8 completion, the playback wait loop, the CG path,
  and standalone-song audio (PROBE5/51/52 must be loud and continuous
  from the first seconds — the disk-image regression of 2026-11-18
  made exactly these play the wrong data). PROBE54/PROBE55 are the
  two probes that do NOT load NEOMSC: they check that code at 0xAB00
  and at 0xA794 runs as written when no NEOMSC was detected
  (docs/APPROXIMATIONS.md item 25). They run their routine with
  BSAVE + `BLOAD ...,R`, because `CALL` does not reach the vessel in
  the host HAL. PROBE56 plants a register recorder at each EEA vector
  and checks the relays' entry registers against the contract
  (A / BC / HL for XDF and XDD, A / BC for XDE); PROBE57 checks that
  a notified load over the NEOMSC body disarms the NEOMSC-only traps.
  A failure here means the behaviour is *wrong*, not
  merely different.
- **Tier B (characterization snapshots)** — the final screen of every
  probe in `tests/probes/` compared against `tests/expected/`. These
  baselines record what the code does today; they are not proofs of
  correctness. A diff means behaviour changed: review it, and if the
  change is intended, re-bless and say why in the commit message.
- **Embed** — the embedded engine boots the real disk: the title CG
  must reach the verified VRAM plane counts [4555, 4770, 5394], the
  audio ring must not drop more than 20,000 frames, and the produced
  audio must follow the genuine phase order (loading BGM through the
  loads, a quiet gap, attract songs loud) as checked by
  `tests/wavprofile.py`. Peaks and phase order do not pin the audio
  *content* (a build playing at twice the tempo passed them), so the
  55 s run also freezes the vessel's diagnostic counters at emulated
  second 50 — Timer B and VRTC deliveries, OPNA register writes by
  class for both banks, ADPCM-B sample reads — and compares them
  exactly with `tests/expected/embed55.counters`. The snapshot point
  is emulated time, so the values do not depend on host scheduling.
  Pin the file from a build known to be correct with

      tests/run_regress.sh --pin-counters

  and review its diff like a baseline (`--bless` does not touch it).
  While the file is missing the suite prints a NOTE and does not
  check the counters. `tests/select_hold_test.sh` (also runnable on
  its own, about 45 s) enters MA2026's song select, holds cursor-down
  for 300 ms and reads the cursor position from the guest text image:
  it must rest on song 2 or 3, the genuine machine's 4-5 moves per
  second (docs/APPROXIMATIONS.md item 35). It runs only when the test
  disk is MA2026A.D88.

The QUASI88 differential check lives outside this suite (it needs
the user's own ROM dump and an X server); see docs/HOST-CLI.md and
the working notes for how it is run.
