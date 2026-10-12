#!/usr/bin/env python3
"""
Isolated verification that load_from_sd() actually reads the right bytes from the
SD card into the SDRAM .appcode region -- pure host-side, no firmware changes.

Flow:
  1. attach to the self-booted target (spinning at the BOOT_TEST gate),
  2. break on psched_init (runs right after load_from_sd(), before the scheduler
     executes anything from SDRAM) and on fault_trap (backstop),
  3. release the boot gate,
  4. on the psched_init halt, read [_appcode_start, _appcode_end) over JTAG and
     diff it against the local build/app.bin (the bytes that SHOULD have landed).

A clean match proves the CMD18/PIO read + addressing in bsp/sd.c works. A fault
(instead of the psched_init halt) means the loaded bytes were garbage -- i.e. the
read is wrong or app.bin isn't at SD_APPCODE_LBA.

Prereqs (card-side, yours): build/app.bin written to the card, and SD_APPCODE_LBA
in bsp/sd.c pointing at that sector. Run: python3 verify_sd.py
"""
from __future__ import annotations

import re
from pathlib import Path

from test import OCD, elf_symbols, TEST_ELF, require_symbols

ROOT    = Path(__file__).resolve().parent
APP_BIN = ROOT / "build" / "app.bin"


def main() -> None:
    if not TEST_ELF.exists():
        raise SystemExit(f"{TEST_ELF} missing -- run `make boot` first")
    if not APP_BIN.exists():
        raise SystemExit(f"{APP_BIN} missing -- run `make boot` first")

    symbols = elf_symbols(TEST_ELF)
    require_symbols(symbols, ("_reset_handler", "psched_init", "fault_trap", "g_fault",
                              "g_error_message", "g_error_ctx", "g_edf_bad_thread",
                              "g_boot_release", "_appcode_start", "_appcode_end"))

    appcode_start = symbols["_appcode_start"]
    region_len    = symbols["_appcode_end"] - appcode_start
    expected      = APP_BIN.read_bytes()

    print(f"appcode region: 0x{appcode_start:08x} .. 0x{symbols['_appcode_end']:08x} "
          f"({region_len} bytes)")
    print(f"app.bin:        {len(expected)} bytes")
    n = min(region_len, len(expected))
    if region_len != len(expected):
        print(f"  NOTE: region/app.bin sizes differ; comparing the first {n} bytes")

    bp_load  = symbols["load_from_sd"]
    bp_fault = symbols["fault_trap"]

    with OCD() as ocd:
        ocd.fault_addr   = symbols["g_fault"]
        ocd.error_addr   = symbols["g_error_message"]
        ocd.ctx_addr     = symbols["g_error_ctx"]
        ocd.edf_bad_addr = symbols["g_edf_bad_thread"]
        ocd.num_cpus     = 2 if symbols.get("_cfg_enable_smp") else 1

        ocd.halt()
        print(f"attached, PC=0x{ocd.pc():08x}")

        # Halt the instant load_from_sd() RETURNS: break its entry, read lr (its
        # return site in cpu0_startup), break there. That point is after the SD
        # read but before cpsie / CPU1 release / psched_init -- no ticks execute
        # the loaded bytes, so the region reads cleanly whatever its contents.
        ocd.add_hw_breakpoint(bp_load)
        ocd.add_hw_breakpoint(bp_fault)
        ocd.write_u32(symbols["g_boot_release"], 1)
        ocd.resume()

        pc = ocd.wait_halt(timeout=30.0)
        if pc != bp_load:
            print(f"\nhalt 0x{pc:08x} != load_from_sd 0x{bp_load:08x} -- faulted before the loader even ran:")
            print(ocd.diagnose())
            raise SystemExit(1)

        # At load_from_sd's ENTRY, SDRAM is up but the read hasn't run. Stamp a
        # sentinel across the region so we can tell "loader wrote nothing" (sentinel
        # survives) from "loader wrote the wrong bytes" (sentinel overwritten).
        SENTINEL = 0xDEADBEEF
        for w in range(64):   # first 256 bytes is plenty to classify
            ocd.write_u32(appcode_start + 4 * w, SENTINEL)

        m = re.search(r"0x([0-9a-fA-F]+)", ocd.cmd("reg lr"))
        ret = int(m.group(1), 16) & ~1
        ocd.remove_hw_breakpoint(bp_load)
        ocd.add_hw_breakpoint(ret)
        ocd.continue_from_breakpoint()

        pc = ocd.wait_halt(timeout=30.0)
        if pc != ret:
            print(f"\nhalt 0x{pc:08x} != load_from_sd return 0x{ret:08x} -- faulted inside the loader:")
            print(ocd.diagnose())
            raise SystemExit(1)

        # Controller state right after the loader returned -- the error path leaves
        # RINTSTS error bits set (read_block returns without clearing on error).
        SDMMC = 0xFF704000
        regs = {name: ocd.read_u32(SDMMC + off) for name, off in
                (("CTRL", 0x00), ("CMD", 0x2C), ("CMDARG", 0x28), ("RINTSTS", 0x44),
                 ("STATUS", 0x48), ("RESP0", 0x30), ("BMOD", 0x80))}
        print("\nSD controller after load_from_sd:")
        for k, v in regs.items():
            print(f"  {k:8} = 0x{v:08x}")
        ERR_BITS = {1: "RE", 6: "RCRC", 7: "DCRC", 8: "RTO", 9: "DRTO", 10: "HTO",
                    11: "FRUN", 12: "HLE", 13: "SBE", 15: "EBE"}
        errs = [name for bit, name in ERR_BITS.items() if regs["RINTSTS"] & (1 << bit)]
        if errs:
            print(f"  RINTSTS error bits: {', '.join(errs)}")

        sentinel_region = ocd.read_words(appcode_start, 64)
        if all(v == SENTINEL for v in sentinel_region):
            print("  SENTINEL INTACT -> load_from_sd wrote NOTHING (read_block erred on "
                  "block 0 and bailed). What you see in SDRAM is leftover/uninitialized DDR.")
        else:
            print("  sentinel overwritten -> the loader did write; bytes are just wrong.")

        # load_from_sd has returned; IRQs still masked, CPU1 not released. Clean read.
        got = ocd.read_bytes(appcode_start, n)

    if got == expected[:n]:
        print(f"\nPASS -- {n} bytes in SDRAM match build/app.bin. SD read path works.")
        return

    # Mismatch. Is what landed actually sector 0 (an MBR ends with 0x55AA at 0x1FE)?
    # If so the driver READ fine -- it just read the wrong sector because
    # SD_APPCODE_LBA is 0 / app.bin isn't flashed there.
    if len(got) >= 0x200 and got[0x1FE] == 0x55 and got[0x1FF] == 0xAA:
        print("\nSD READ WORKS, WRONG SECTOR -- the region holds an MBR (0x55AA @0x1FE), "
              "i.e. load_from_sd correctly read sector 0. Set SD_APPCODE_LBA to app.bin's "
              "sector and flash it; the CMD18 path is fine.")
        raise SystemExit(2)

    # First mismatch, for diagnosis.
    off = next(i for i in range(n) if got[i] != expected[i])
    lo  = off & ~0xF
    exp = expected[lo:lo + 16].hex(" ")
    act = got[lo:lo + 16].hex(" ")
    diff = sum(1 for i in range(n) if got[i] != expected[i])
    print(f"\nFAIL -- {diff}/{n} bytes differ; first at offset 0x{off:x} "
          f"(SDRAM 0x{appcode_start + off:08x}):")
    print(f"  expected @0x{lo:x}: {exp}")
    print(f"  got      @0x{lo:x}: {act}")
    raise SystemExit(1)


if __name__ == "__main__":
    main()
