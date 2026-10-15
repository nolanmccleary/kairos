#!/usr/bin/env python3
"""Render the uncached memory-access latency figure from a memlat.csv summary.

The per-run CSV holds summary stats (min/mean/p50/p99/max) per case, not raw
samples, so we draw a shared-axis range plot: every case on ONE cycle axis so
OCRAM vs SDRAM magnitudes are directly comparable and the determinism contrast
(SDRAM reads have a long tail; everything else is a near-point) is obvious.

Usage: python3 scripts/plot_memlat.py <memlat.csv> <out.png>
"""
import csv
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


def load(csv_path: Path) -> tuple[dict[str, dict[str, float]], int]:
    rows, baseline = {}, 0
    with csv_path.open() as f:
        for r in csv.DictReader(f):
            name = r["distribution"]
            if name == "baseline_cycles":
                baseline = int(r["count"])
                continue
            rows[name] = {k: float(r[k]) for k in ("min", "mean", "p99", "max")}
            rows[name]["median"] = float(r.get("median", r.get("p50")))   # accept older p50-headed CSVs
    return rows, baseline


def main() -> None:
    csv_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    rows, baseline = load(csv_path)

    # OCRAM then SDRAM within each op, so each read/write pair is adjacent.
    order = [("ocram_read", "OCRAM read"), ("sdram_read", "SDRAM read"),
             ("ocram_write", "OCRAM write"), ("sdram_write", "SDRAM write")]
    OC, SD = "#1f77b4", "#d62728"
    color = {"ocram_read": OC, "ocram_write": OC, "sdram_read": SD, "sdram_write": SD}

    fig, ax = plt.subplots(figsize=(11, 4.2))
    for row, (key, label) in enumerate(order):
        s = rows[key]
        y = len(order) - 1 - row
        c = color[key]
        # full range min..max (thin, the tail), bulk min..p99 (thick).
        ax.plot([s["min"], s["max"]], [y, y], color=c, lw=1.2, alpha=0.45, zorder=1)
        ax.plot([s["min"], s["p99"]], [y, y], color=c, lw=7.0, alpha=0.30,
                solid_capstyle="butt", zorder=2)
        ax.scatter([s["median"]], [y], color=c, s=55, zorder=4, label="_median")  # median
        ax.scatter([s["mean"]], [y], color="black", marker="|", s=240, zorder=5)  # mean
        ax.scatter([s["max"]], [y], color=c, marker="D", s=22, zorder=4)          # tail max

        ax.annotate(f"min {int(s['min'])}", (s["min"], y), textcoords="offset points",
                    xytext=(-6, 9), ha="right", fontsize=8, color=c)
        ax.annotate(f"mean {int(round(s['mean']))}", (s["mean"], y), textcoords="offset points",
                    xytext=(0, 22), ha="center", fontsize=8, color="black")   # stacked above min
        ax.annotate(f"median {int(round(s['median']))}", (s["median"], y), textcoords="offset points",
                    xytext=(0, -15), ha="center", fontsize=8, color=c)
        if s["max"] - s["min"] > 4:   # only label the tail when there is one
            ax.annotate(f"p99 {int(s['p99'])} / max {int(s['max'])}", (s["max"], y),
                        textcoords="offset points", xytext=(8, 9), ha="left", fontsize=8, color=c)

    ax.set_yticks(range(len(order)))
    ax.set_yticklabels([lbl for _, lbl in reversed([(k, l) for k, l in order])], fontsize=10)
    ax.set_ylim(-0.6, len(order) - 0.4)
    ax.set_xlabel("latency (CPU cycles, uncached single word)")
    ax.margins(x=0.08)
    ax.grid(axis="x", alpha=0.3)
    ax.set_title("Uncached access latency: OCRAM vs SDRAM\n"
                 "OCRAM and SDRAM writes are near-deterministic; SDRAM reads carry a long tail",
                 fontsize=11)

    legend = [Line2D([0], [0], marker="o", color="w", markerfacecolor="gray",
                     markersize=8, label="median"),
              Line2D([0], [0], marker="|", color="black", markersize=12, label="mean"),
              Line2D([0], [0], marker="D", color="w", markerfacecolor="gray",
                     markersize=6, label="max"),
              Line2D([0], [0], color="gray", lw=7, alpha=0.3, label="min..p99 (bulk)"),
              Line2D([0], [0], color="gray", lw=1.2, alpha=0.45, label="min..max (full range)")]
    ax.legend(handles=legend, fontsize=8, loc="lower right", framealpha=0.9)
    fig.text(0.01, 0.01, f"baseline (pmu+dsb overhead) subtracted elsewhere: {baseline} cyc; "
             f"source: {csv_path.name}", fontsize=7, color="#666")

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
