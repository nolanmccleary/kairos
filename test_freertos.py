#!/usr/bin/env python3
"""FreeRTOS battery driver. Runs the one FreeRTOS binary (scheduler up, every bench a
FreeRTOS task) and plots:
  - heap_4 alloc/free cost vs payload size and vs free-list length (fragmentation),
  - FreeRTOS scheduler cost vs loaded task count (per-tick xTaskIncrementTick).

Build first:  make FREERTOS=1 build/test.elf   (or: make boot FREERTOS=1)
Then:         python3 test_freertos.py          (or --bootable)

The Kairos counterpart is test.py; this is the FreeRTOS battery. It reuses only the
generic JTAG plumbing and the shared collectors/reducers/plotters from test.py (the bench
symbols differ per battery); it adds no FreeRTOS phase to test.py itself.
"""
from __future__ import annotations

import argparse
import csv
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

from test import (OCD, elf_symbols, require_symbols,
                  collect_sweep, plot_sweep, write_sweep_csv, ALLOC_SWEEPS,
                  dist_stats, SCHED_SAMPLES)

ROOT        = Path(__file__).resolve().parent
TEST_ELF    = ROOT / "build" / "test.elf"
RESULTS_DIR = ROOT / "test_results"


def schedsweep_test(ocd: OCD, symbols: dict[str, int]) -> list[tuple[int, list[int]]]:
    """Continue from the alloc-done breakpoint to the scheduler-sweep-done breakpoint,
    then read the per-N per-tick cost distributions (g_frtos_sched_*)."""
    bp_done = symbols["ktrace_bp_sched_sweep_done"]
    ocd.add_hw_breakpoint(bp_done)

    # The sweep runs 7 levels x 512 ticks under the live scheduler (~a few seconds). Poll.
    ocd.continue_from_breakpoint()
    deadline = time.time() + 90.0
    while True:
        time.sleep(2.0)
        ocd.halt()
        if ocd.pc() == bp_done:
            break
        if time.time() > deadline:
            raise RuntimeError(f"freertos schedsweep stalled, pc=0x{ocd.pc():08x}:\n{ocd.diagnose()}")
        ocd.resume()

    n      = min(ocd.read_u32(symbols["g_frtos_sched_n"]), 32)
    levels = ocd.read_words(symbols["g_frtos_sched_levels"], n)
    counts = ocd.read_words(symbols["g_frtos_sched_counts"], n)
    base   = symbols["g_frtos_sched_samples"]

    rows: list[tuple[int, list[int]]] = []
    for i in range(n):
        c = min(counts[i], SCHED_SAMPLES)
        samples = ocd.read_words(base + i * SCHED_SAMPLES * 4, c) if c else []
        rows.append((levels[i], samples))

    print("\nFreeRTOS scheduler overhead vs loaded tasks (per-tick xTaskIncrementTick, cycles):")
    for N, samples in rows:
        s = dist_stats(samples)
        print(f"  N={N:<3} n={int(s['count']):<4} min={int(s['min']):<5} mean={s['mean']:<7.1f} "
              f"median={int(s['median']):<5} p99={int(s['p99']):<5} max={int(s['max'])}")
    return rows


def write_schedsweep_csv(path: Path, rows: list[tuple[int, list[int]]]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["distribution", "count", "min", "mean", "median", "p99", "max", "unit"])
        for N, samples in rows:
            s = dist_stats(samples)
            w.writerow([f"{N} tasks", int(s["count"]), int(s["min"]), round(s["mean"], 1),
                        int(s["median"]), int(s["p99"]), int(s["max"]), "cycles"])


def main(bootable: bool = False) -> None:
    if not TEST_ELF.exists():
        raise RuntimeError(f"{TEST_ELF} does not exist -- build it with: make FREERTOS=1 build/test.elf")

    symbols = elf_symbols(TEST_ELF)
    if "pvPortMalloc" not in symbols:
        raise RuntimeError("this elf has no pvPortMalloc -- not a FreeRTOS build. Use: make FREERTOS=1")
    require_symbols(symbols, ("_reset_handler", "ktrace_bp_alloc_done", "ktrace_bp_sched_sweep_done",
                              "g_allocsweep_n", "g_fragsweep_n", "g_frtos_sched_n"))
    if bootable:
        require_symbols(symbols, ("g_boot_release",))

    outdir = RESULTS_DIR / datetime.now().strftime("%Y%m%d-%H%M%S-freertos")
    outdir.mkdir(parents=True, exist_ok=True)
    print(f"artifacts -> {outdir}\n")

    with OCD() as ocd:
        if bootable:
            ocd.halt()
            print(f"bootable: attached to self-booted target, PC=0x{ocd.pc():08x}")
        else:
            ocd.load_image(TEST_ELF, symbols["_reset_handler"])

        bp = symbols["ktrace_bp_alloc_done"]
        ocd.add_hw_breakpoint(bp)
        if bootable:
            ocd.write_u32(symbols["g_boot_release"], 1)
        ocd.resume()
        ocd.expect_breakpoint(bp, timeout=30.0)

        sweeps = {}
        for key, (n_sym, x_sym, a_sym, f_sym, *_rest) in ALLOC_SWEEPS.items():
            if symbols.get(n_sym):
                sweeps[key] = collect_sweep(ocd, symbols, n_sym, x_sym, a_sym, f_sym)

        sched_rows = schedsweep_test(ocd, symbols)

    for key, sweep in sweeps.items():
        _ns, _xs, _as, _fs, xname, title_suf, xlabel, stem = ALLOC_SWEEPS[key]
        print(f"FreeRTOS heap_4 {title_suf} (cycles):")
        for i, xv in enumerate(sweep.x):
            print(f"  {xv:>7}   alloc mean={sweep.alloc[i].mean:<6} min={sweep.alloc[i].minimum:<6}"
                  f"   free mean={sweep.free[i].mean:<6} min={sweep.free[i].minimum}")
        write_sweep_csv(outdir / f"{stem}_heap4.csv", sweep, xname)
        plot_sweep(sweep, outdir / f"{stem}_heap4.png",
                   f"Allocator cost {title_suf}: FreeRTOS heap_4", xlabel)

    write_schedsweep_csv(outdir / "schedsweep_freertos.csv", sched_rows)
    try:
        subprocess.run([sys.executable, str(ROOT / "scripts" / "plot_latency_ranges.py"),
                        str(outdir / "schedsweep_freertos.csv"),
                        str(outdir / "schedsweep_freertos.png"),
                        "FreeRTOS scheduler overhead vs loaded task count (per-tick, cycles)"],
                       check=True)
    except Exception as e:
        print(f"(schedsweep plot skipped: {e})")

    print(f"\nPASS   artifacts in {outdir}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="FreeRTOS battery driver (heap_4 sweeps + scheduler sweep)")
    parser.add_argument("--bootable", "--live", action="store_true", dest="bootable",
                        help="attach to a self-booted target and release the boot gate "
                             "instead of load_image-ing the ELF over JTAG")
    args = parser.parse_args()
    main(bootable=args.bootable)
