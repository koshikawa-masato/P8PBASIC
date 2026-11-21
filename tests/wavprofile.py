#!/usr/bin/env python3
"""Per-second peak profiler for P8PBASIC test WAVs, with phase checks.

Usage:
  wavprofile.py peaks FILE.wav      print "sec peak" per second
  wavprofile.py probe FILE.wav      a standalone-song probe (PROBE5/51/52):
                                    the song must start within 5 s and then
                                    peak >= 3000 for 10 straight seconds
                                    (PROBE52 spends its first ~2 s on the
                                    banner XDD draws before XDF P; what
                                    matters is that once the song starts it
                                    is loud and continuous -- the disk-image
                                    regression made it play wrong data)
  wavprofile.py embed FILE.wav      the full MA2026 embed run (55 s):
                                    the genuine order must hold --
                                    * >= 20 loud seconds (>= 3000) in 3..31
                                      (loading BGM through the disk loads;
                                      the first attract song also lands in
                                      this window)
                                    * some second in 21..32 below 3000
                                      (the quiet gap between BGM stop and
                                      the first attract song: CG/comment
                                      loads, the song-list draw and the
                                      first song load)
                                    * >= 5 loud seconds in 30..44
                                      (attract songs at full volume)
                                    Windows re-derived 2026-11-21 after the
                                    per-statement pacing fix (item 6):
                                    measured BGM 1..22, gap 23..28, first
                                    song from 29 in the 40 s run.

Exit status 0 = pass, 1 = fail (reasons on stdout). Peaks are absolute
sample maxima over both channels of a 44.1 kHz 16-bit stereo WAV.
"""
import sys
import wave


def peaks(path):
    w = wave.open(path, "rb")
    n = w.getnframes()
    sr = w.getframerate()
    ch = w.getnchannels()
    raw = w.readframes(n)
    w.close()
    import struct
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    out = []
    secs = n // sr
    for s in range(secs):
        seg = samples[s * sr * ch:(s + 1) * sr * ch]
        out.append(max((abs(x) for x in seg), default=0))
    return out


def main():
    mode, path = sys.argv[1], sys.argv[2]
    p = peaks(path)
    if mode == "peaks":
        for i, v in enumerate(p):
            print(i, v)
        return 0
    fails = []
    if mode == "probe":
        onset = next((i for i, v in enumerate(p) if v >= 3000), None)
        if onset is None:
            fails.append("song never gets loud")
        elif onset > 5:
            fails.append("song starts too late: onset at %d s" % onset)
        else:
            for s in range(onset, onset + 10):
                if s >= len(p) or p[s] < 3000:
                    fails.append("second %d peak %s < 3000 (song not loud/continuous)"
                                 % (s, p[s] if s < len(p) else "EOF"))
                    break
    elif mode == "embed":
        loud = lambda s: s < len(p) and p[s] >= 3000
        n_bgm = sum(1 for s in range(3, 32) if loud(s))
        if n_bgm < 20:
            fails.append("loading-BGM window: only %d loud seconds in 3..31 "
                         "(want >= 20)" % n_bgm)
        if not any(s < len(p) and p[s] < 3000 for s in range(21, 33)):
            fails.append("no quiet second in 21..32 (BGM/attract gap missing)")
        n_att = sum(1 for s in range(30, 45) if loud(s))
        if n_att < 5:
            fails.append("attract window: only %d loud seconds in 30..44 "
                         "(want >= 5)" % n_att)
    else:
        print("unknown mode", mode)
        return 2
    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("OK (%s): %d seconds profiled" % (mode, len(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
