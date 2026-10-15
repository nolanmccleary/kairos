#!/usr/bin/env python3
"""FreeRTOS binary driver (one job): run the heap_4 allocator sweeps and plot alloc/free
cost vs payload size and vs free-list length (fragmentation).

Build the FreeRTOS binary first:  make FREERTOS=1 build/test.elf   (or: make boot FREERTOS=1)
Then:                             python3 test_freertos.py          (or --bootable)

Separate from test.py on purpose. It reuses only the generic JTAG plumbing and the
allocator-sweep collector/plot from test.py (the sweep symbols are identical in both
binaries); it adds no FreeRTOS phase to test.py itself.
"""
from __future__ import annotations

import argparse
from datetime import datetime
from pathlib import Path

from test import (OCD, elf_symbols, require_symbols,
                  collect_sweep, plot_sweep, write_sweep_csv, ALLOC_SWEEPS)

ROOT        = Path(__file__).resolve().parent
TEST_ELF    = ROOT / "build" / "test.elf"
RESULTS_DIR = ROOT / "test_results"


def main(bootable: bool = False) -> None:
    if not TEST_ELF.exists():
        raise RuntimeError(f"{TEST_ELF} does not exist -- build it with: make FREERTOS=1 build/test.elf")

    symbols = elf_symbols(TEST_ELF)
    if "pvPortMalloc" not in symbols:
        raise RuntimeError("this elf has no pvPortMalloc -- not a FreeRTOS build. Use: make FREERTOS=1")
    require_symbols(symbols, ("_reset_handler", "ktrace_bp_alloc_done",
                              "g_allocsweep_n", "g_fragsweep_n"))
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

    for key, sweep in sweeps.items():
        _ns, _xs, _as, _fs, xname, title_suf, xlabel, stem = ALLOC_SWEEPS[key]
        print(f"FreeRTOS heap_4 {title_suf} (cycles):")
        for i, xv in enumerate(sweep.x):
            print(f"  {xv:>7}   alloc mean={sweep.alloc[i].mean:<6} min={sweep.alloc[i].minimum:<6}"
                  f"   free mean={sweep.free[i].mean:<6} min={sweep.free[i].minimum}")
        write_sweep_csv(outdir / f"{stem}_heap4.csv", sweep, xname)
        plot_sweep(sweep, outdir / f"{stem}_heap4.png",
                   f"Allocator cost {title_suf}: FreeRTOS heap_4", xlabel)

    print(f"\nPASS   artifacts in {outdir}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="FreeRTOS heap_4 allocator-sweep driver")
    parser.add_argument("--bootable", "--live", action="store_true", dest="bootable",
                        help="attach to a self-booted target and release the boot gate "
                             "instead of load_image-ing the ELF over JTAG")
    args = parser.parse_args()
    main(bootable=args.bootable)
