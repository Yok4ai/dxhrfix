#!/usr/bin/env python3
"""Frame time chart from MangoHud logs, one panel per run.

Usage: python3 -I tools/frametime_chart.py out.png "Title=file.csv" ["Title=file.csv" ...]

Each panel plots frame time (ms) against elapsed time (s) for one run, clipped
at CLIP_MS, with a reference line at 50 ms. The first SKIP_S seconds are shaded
and left out of the counts in the panel titles, matching frametime_stats.py.
Needs matplotlib.
"""
import csv, os, sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CLIP_MS = 200
SKIP_S = 20.0
SURFACE, INK, INK2, GRID, SERIES = "#fcfcfb", "#0b0b0b", "#52514e", "#e6e5e1", "#2a78d6"

def load(path):
    with open(os.path.expanduser(path), newline="") as f:
        rows = list(csv.reader(f))
    hdr = next(i for i, r in enumerate(rows) if r and r[0] == "fps" and "frametime" in r)
    ift = rows[hdr].index("frametime")
    ft, el, t = [], [], 0.0
    for r in rows[hdr + 1:]:
        try:
            v = float(r[ift])
        except (ValueError, IndexError):
            continue
        t += v
        ft.append(v)
        el.append(t / 1000.0)
    return el, ft

def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    out, runs = argv[0], argv[1:]
    fig, axes = plt.subplots(len(runs), 1, figsize=(9, 2.2 * len(runs)), sharex=True, facecolor=SURFACE)
    if len(runs) == 1:
        axes = [axes]
    for ax, arg in zip(axes, runs):
        title, _, path = arg.partition("=")
        el, ft = load(path)
        play = [v for e, v in zip(el, ft) if e >= SKIP_S]
        over = sum(1 for v in play if v > 50)
        ax.set_facecolor(SURFACE)
        ax.axvspan(0, SKIP_S, color=GRID, alpha=0.6, linewidth=0)
        ax.plot(el, [min(v, CLIP_MS) for v in ft], color=SERIES, linewidth=0.8)
        ax.axhline(50, color=INK2, linewidth=0.8, linestyle=(0, (4, 4)))
        ax.set_ylim(0, CLIP_MS)
        ax.set_yticks([0, 50, 100, 150, 200])
        ax.set_yticklabels(["0", "50", "100", "150", "200+"])
        ax.set_ylabel("ms", color=INK2)
        ax.set_title(f"{title}: {over} frame{"" if over == 1 else "s"} over 50 ms after load-in, worst {max(play):.0f} ms", loc="left", fontsize=10, color=INK)
        ax.grid(axis="y", color=GRID, linewidth=0.6)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        for s in ("left", "bottom"):
            ax.spines[s].set_color(GRID)
        ax.tick_params(colors=INK2, labelsize=8)
    axes[0].text(SKIP_S / 2, CLIP_MS * 0.9, "load-in", ha="center", va="top", fontsize=8, color=INK2)
    axes[-1].set_xlabel("seconds since logging started (shaded: first 20 s, excluded from counts)", color=INK2)
    fig.tight_layout()
    fig.savefig(out, dpi=130, facecolor=SURFACE)
    print("wrote", out)

if __name__ == "__main__":
    main(sys.argv[1:])
