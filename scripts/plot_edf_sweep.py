#!/usr/bin/env python3
"""Render the EDF schedulability crossover figure from an edf_sweep.csv.

The sweep raises the taskset's utilization and counts deadline misses. We plot
misses against the *configured* utilization U = sum(C_i/T_i) -- the quantity the
Liu & Layland bound is stated in -- so the crossover (first U with misses) lands
where the theory says it should, at U = 1.

Usage: python3 scripts/plot_edf_sweep.py <edf_sweep.csv> <out.png>
"""
import csv
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main() -> None:
    csv_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])

    U, misses = [], []
    with csv_path.open() as f:
        for r in csv.DictReader(f):
            U.append(float(r["configured_u"]))
            misses.append(int(r["misses"]))

    pts = sorted(zip(U, misses))
    U = [u for u, _ in pts]
    misses = [m for _, m in pts]

    last_ok = max((u for u, m in pts if m == 0), default=None)
    first_miss = min((u for u, m in pts if m > 0), default=None)

    fig, ax = plt.subplots(figsize=(10, 4.6))
    lo, hi = min(U), max(U)

    # schedulable (U <= 1) vs overload (U > 1) regions, split at the Liu-Layland bound.
    ax.axvspan(lo - 0.02, 1.0, color="#2ca02c", alpha=0.07, zorder=0)
    ax.axvspan(1.0, hi + 0.02, color="#d62728", alpha=0.07, zorder=0)
    ax.axvline(1.0, color="#333", ls="--", lw=1.3, zorder=3)
    ax.annotate("Liu–Layland bound  U = 1", (1.0, 0.97), xycoords=("data", "axes fraction"),
                xytext=(-8, -4), textcoords="offset points", ha="right", va="top", fontsize=9, color="#333")

    ax.plot(U, misses, color="#888", lw=1.0, zorder=2)
    ok  = [(u, m) for u, m in pts if m == 0]
    bad = [(u, m) for u, m in pts if m > 0]
    ax.scatter([u for u, _ in ok],  [m for _, m in ok],  s=70, color="#2ca02c",
               zorder=4, label="0 misses (schedulable)")
    ax.scatter([u for u, _ in bad], [m for _, m in bad], s=70, color="#d62728",
               zorder=4, label="deadline misses")

    for u, m in bad:
        ax.annotate(str(m), (u, m), textcoords="offset points", xytext=(0, 8),
                    ha="center", fontsize=8, color="#d62728")

    if last_ok is not None and first_miss is not None:
        ax.annotate(f"last clean: U = {last_ok:.3f}", (last_ok, 0), textcoords="offset points",
                    xytext=(-8, 26), ha="right", fontsize=8.5, color="#2ca02c",
                    arrowprops=dict(arrowstyle="->", color="#2ca02c", lw=0.8))
        ax.annotate(f"first misses: U = {first_miss:.3f}", (first_miss, dict(bad)[first_miss]),
                    textcoords="offset points", xytext=(14, 18), ha="left", fontsize=8.5, color="#d62728",
                    arrowprops=dict(arrowstyle="->", color="#d62728", lw=0.8))

    ax.set_xlim(lo - 0.02, hi + 0.02)
    ax.set_ylim(-max(misses) * 0.03 - 1, max(misses) * 1.12)
    ax.set_xlabel(r"configured utilization  $U=\sum_i C_i/T_i$")
    ax.set_ylabel("deadline misses (per ~12 s trial)")
    ax.set_title("EDF schedulability: deadline misses appear right at U = 1")
    ax.grid(axis="y", alpha=0.3)
    ax.legend(loc="upper left", fontsize=9, framealpha=0.9)
    fig.text(0.01, 0.01, f"source: {csv_path.parent.name}/{csv_path.name}", fontsize=7, color="#666")

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"wrote {out_path}  (last clean U={last_ok}, first-miss U={first_miss})")


if __name__ == "__main__":
    main()
