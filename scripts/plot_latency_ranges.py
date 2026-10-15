#!/usr/bin/env python3
"""Shared-axis latency range plot from a summary CSV (min/mean/median/p99/max rows).

Matches the memlat figure's visual language: every distribution on ONE cycle
axis, each drawn as min..max (thin, the tail) + min..p99 (thick, the bulk) with
median, mean, and max markers (each labelled with its value). Good for the
microbench CSVs (ctxsw, irqlat), which store summary stats rather than raw samples.

Usage: python3 scripts/plot_latency_ranges.py <in.csv> <out.png> "<title>"
"""
import csv
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

PALETTE = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e"]
STATS = ("min", "mean", "p99", "max")


def load(csv_path: Path):
    dists, meta = [], {}
    with csv_path.open() as f:
        for r in csv.DictReader(f):
            name = (r.get("distribution") or "").strip()
            if not name:
                continue
            median = r.get("median", r.get("p50"))   # accept older p50-headed CSVs
            try:
                s = {k: float(r[k]) for k in STATS}
                s["median"] = float(median)
            except (TypeError, ValueError):
                # metadata rows (e.g. switch_fraction_p,0.3669) reuse the first two cols.
                meta[name] = r.get("count")
                continue
            dists.append((name, s))
    return dists, meta


def main() -> None:
    csv_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    title = sys.argv[3] if len(sys.argv) > 3 else csv_path.stem

    dists, meta = load(csv_path)
    n = len(dists)

    fig, ax = plt.subplots(figsize=(11, 1.15 * n + 2.1))
    for row, (name, s) in enumerate(dists):
        y = n - 1 - row
        c = PALETTE[row % len(PALETTE)]
        ax.plot([s["min"], s["max"]], [y, y], color=c, lw=1.2, alpha=0.45, zorder=1)
        ax.plot([s["min"], s["p99"]], [y, y], color=c, lw=8.0, alpha=0.30,
                solid_capstyle="butt", zorder=2)
        ax.scatter([s["median"]], [y], color=c, s=60, zorder=4)                    # median
        ax.scatter([s["mean"]], [y], color="black", marker="|", s=260, zorder=5)   # mean
        ax.scatter([s["max"]], [y], color=c, marker="D", s=24, zorder=4)           # tail max

        ax.annotate(f"min {int(s['min'])}", (s["min"], y), textcoords="offset points",
                    xytext=(-6, 10), ha="right", fontsize=8, color=c)
        ax.annotate(f"mean {int(round(s['mean']))}", (s["mean"], y), textcoords="offset points",
                    xytext=(0, 22), ha="center", fontsize=8, color="black")   # stacked above min
        ax.annotate(f"median {int(round(s['median']))}", (s["median"], y), textcoords="offset points",
                    xytext=(0, -17), ha="center", fontsize=8, color=c)
        if s["max"] - s["min"] > 4:
            ax.annotate(f"p99 {int(s['p99'])} / max {int(s['max'])}", (s["max"], y),
                        textcoords="offset points", xytext=(8, 10), ha="left", fontsize=8, color=c)

    ax.set_yticks(range(n))
    ax.set_yticklabels([name for name, _ in reversed(dists)], fontsize=11)
    ax.set_ylim(-0.6, n - 0.4)
    ax.set_xlabel("cycles")
    ax.margins(x=0.10)
    ax.grid(axis="x", alpha=0.3)
    ax.set_title(title, fontsize=11)

    legend = [Line2D([0], [0], marker="o", color="w", markerfacecolor="gray", markersize=8, label="median"),
              Line2D([0], [0], marker="|", color="black", markersize=12, label="mean"),
              Line2D([0], [0], marker="D", color="w", markerfacecolor="gray", markersize=6, label="max"),
              Line2D([0], [0], color="gray", lw=8, alpha=0.3, label="min..p99 (bulk)"),
              Line2D([0], [0], color="gray", lw=1.2, alpha=0.45, label="min..max (full range)")]
    ax.legend(handles=legend, fontsize=8, loc="lower right", framealpha=0.9, ncol=2)

    note = f"source: {csv_path.parent.name}/{csv_path.name}"
    if "switch_fraction_p" in meta:
        note += f"   |   switch fraction p = {meta['switch_fraction_p']}"
    fig.text(0.01, 0.01, note, fontsize=7, color="#666")

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
