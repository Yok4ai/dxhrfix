#!/usr/bin/env python3
"""Hitch statistics from MangoHud frame time logs.

Usage: python3 -I tools/frametime_stats.py LABEL=file.csv [LABEL=file.csv ...]

Reads MangoHud CSV logs recorded with log_interval=0 (one row per frame) and
prints, per run: play time, frames over 50 ms per minute of play, frames over
100 ms, worst frame and median frame time. The first 20 s after logging starts
are excluded (SKIP_S), because that window is dominated by pipeline compiles on
every build. Read-only.
"""
import csv, os, sys

SKIP_S = 20.0

def load(path):
    with open(os.path.expanduser(path), newline="") as f:
        rows = list(csv.reader(f))
    hdr = next((i for i, r in enumerate(rows) if r and r[0] == "fps" and "frametime" in r), None)
    if hdr is None:
        sys.exit("no fps,frametime header in " + path)
    ift = rows[hdr].index("frametime")
    ft = []
    for r in rows[hdr + 1:]:
        try:
            ft.append(float(r[ift]))
        except (ValueError, IndexError):
            pass
    return ft

def stats(ft):
    t, start = 0.0, 0
    for k, v in enumerate(ft):
        t += v
        if t >= SKIP_S * 1000:
            start = k
            break
    play = ft[start:]
    secs = sum(play) / 1000.0
    play_sorted = sorted(play)
    return {
        "play_s": secs,
        "frames": len(play),
        "over50_per_min": sum(1 for v in play if v > 50) / (secs / 60) if secs else 0.0,
        "over100": sum(1 for v in play if v > 100),
        "worst_ms": max(play) if play else 0.0,
        "median_ms": play_sorted[len(play) // 2] if play else 0.0,
        "p99_ms": play_sorted[int(len(play) * 0.99)] if play else 0.0,
    }

def main(argv):
    if not argv:
        sys.exit(__doc__)
    print("run\tplay_s\tframes\tover50/min\tover100\tworst_ms\tmedian_ms\tp99_ms")
    for arg in argv:
        label, _, path = arg.partition("=")
        if not path:
            label, path = os.path.basename(arg), arg
        s = stats(load(path))
        print(f"{label}\t{s['play_s']:.0f}\t{s['frames']}\t{s['over50_per_min']:.1f}\t{s['over100']}\t{s['worst_ms']:.0f}\t{s['median_ms']:.2f}\t{s['p99_ms']:.1f}")

if __name__ == "__main__":
    main(sys.argv[1:])
