# Kairos

A real-time kernel for the ARM Cortex-A9, built for the Terasic DE1-SoC (Cyclone V).
Dual-core SMP, a hybrid earliest-deadline-first plus round-robin scheduler, a TLSF
allocator, and its own boot path (no U-Boot, no Linux): the kernel runs from on-chip RAM
and the board boots off a microSD card.

This README covers building and running. The design details are in [`writeup.md`](writeup.md).




```bash
# Kairos battery, loaded over JTAG (no SD card needed)
make build/test.elf
python3 test.py            # starts OpenOCD, loads the ELF, runs, writes figures to test_results/

# FreeRTOS battery (head-to-head comparison), same flow
make FREERTOS=1 build/test.elf
python3 test_freertos.py
```

## Test batteries

There are two test batteries, each with its own binary and driver script:

| Battery  | Build                           | Driver              | Scheduler / allocator        |
|----------|----------------------------------|---------------------|------------------------------|
| Kairos   | `make build/test.elf`            | `python3 test.py`   | Kairos EDF+RR / TLSF         |
| FreeRTOS | `make FREERTOS=1 build/test.elf` | `python3 test_freertos.py` | FreeRTOS fixed-priority / heap_4 |

Both build from the same source tree and run on the same chip, so the numbers compare
directly. The FreeRTOS battery vendors FreeRTOS-Kernel V11.1.0 (MIT).

## Prerequisites

### Hardware
- Terasic DE1-SoC (Cyclone V) board
- Its on-board USB-Blaster II JTAG (a USB cable to the host)
- A microSD card (only needed for the self-boot flow, not for JTAG-loaded runs)

### Software
- ARM bare-metal toolchain: `arm-none-eabi-gcc` (plus `objcopy`/`objdump`/`nm`). The build
  is GCC-only and does not use newlib.
- OpenOCD (drives the USB-Blaster II)
- Python 3 with matplotlib (the drivers collect telemetry and render figures)
- u-boot-tools for `mkimage` (only needed for `make boot`)
- cppcheck, optionally, for `make lint`

```bash
# macOS
brew install --cask gcc-arm-embedded
brew install open-ocd u-boot-tools
pip3 install matplotlib

# Debian/Ubuntu
sudo apt install gcc-arm-none-eabi openocd u-boot-tools python3-matplotlib
```

The OpenOCD config and USB-Blaster II firmware live in [`openocd/`](openocd/)
(`de1soc.cfg`), and the test drivers launch OpenOCD against it for you. If the blaster is
wedged (first plug-in, or after a bad run), power-cycle it.

## Building

```bash
make build/test.elf              # Kairos battery
make FREERTOS=1 build/test.elf   # FreeRTOS battery
make clean                       # remove build/
```

The ELF is self-contained: the kernel runs from OCRAM and the application/test code is
placed in SDRAM. `build/test.dis` is the disassembly for the last build.

### Build configuration

Hardware features and the test payload are compile-time flags (pass them on the `make`
line, e.g. `make ENABLE_SMP=0 build/test.elf`). They are also emitted into the ELF symbol
table, so the host reads the exact config back from the image.

| Flag                | Default | Meaning                                                        |
|---------------------|---------|----------------------------------------------------------------|
| `ENABLE_MMU`        | `1`     | MMU + page tables (required for D-cache, L2, SMP)              |
| `ENABLE_DCACHE`     | `1`     | L1 data cache                                                  |
| `ENABLE_ICACHE`     | `1`     | L1 instruction cache                                           |
| `ENABLE_L2`         | `1`     | PL310 outer (L2) cache                                         |
| `ENABLE_SMP`        | `1`     | bring up CPU1 and the SCU; single-core if `0`                 |
| `ENABLE_NULL_GUARD` | `1`     | unmap the low 1 MB so NULL / near-NULL derefs fault           |
| `FREERTOS`          | `0`     | build the FreeRTOS battery instead of Kairos                  |
| `BOOT_TEST`         | `0`     | compile in the self-boot gate (set by `make boot`)            |
| `BOARD`             | `cyclone_v` | `cyclone_v` (real HW) or `qemu` (vexpress-a9)             |

The cache/MMU/SMP flags are not independent. The dependencies come from the A9 and are
enforced as `#error` guards in `bsp/bsp.c`, so an invalid combination fails the build:

- **The MMU must be on to run at all.** With it off there are no page tables, so every
  access is Strongly-Ordered. That faults on unaligned accesses (the compiler folds even
  alignment-safe byte reads, e.g. the MBR parse in `load_from_sd`, into a single unaligned
  `ldr`) and makes `LDREX`/`STREX` unusable. An MMU-off image builds but can't run, so
  every runnable config has the MMU on.
- **The D-cache must be on in every runnable config.** Under SMP the exclusive lock
  (`kernel/mutex.s`) needs cacheable memory, because the A9's exclusive monitor is tied to
  the cache line. That's enforced by the `ENABLE_SMP` guard below. Single-core uses a plain
  flag lock (`lock.c`'s `!ENABLE_SMP` path, no exclusives), so in principle it has no
  cacheable requirement, but an all-uncached single-core image does not run the battery in
  practice (observed on HW). So the D-cache is on in every runnable state below, and the
  I-cache is the only cache you can toggle.
- **`ENABLE_DCACHE` needs `ENABLE_MMU`** (build-time): cacheability is a per-page MMU
  attribute, so with the MMU off there's nothing to mark cacheable. (Instruction fetch can
  be cached with the MMU off, which is why `ENABLE_ICACHE` is the exception and stays free.)
- **`ENABLE_L2` needs `ENABLE_MMU`**: the PL310 only caches MMU-marked outer-cacheable memory.
- **`ENABLE_SMP` needs `ENABLE_MMU`, `ENABLE_DCACHE`, `ENABLE_L2`, and the SCU.** Cross-core
  `LDREX`/`STREX` work by the SCU tracking the lock line in the L1 data cache over
  Normal/Shareable/Cacheable memory, so the lock must be cached and coherent. The SCU comes
  up exactly when `ENABLE_SMP` is set (not a separate flag), and the I-cache is irrelevant.
  L2 is not optional under SMP on this SoC (verified on HW): with the PL310 disabled,
  contended cross-core coherent/exclusive writes take the SCU->PL310->L3 path out to the
  fabric and async-abort, while with L2 enabled they resolve in the coherency layer.
  (Single-threaded code is fine with L2 off, because its exclusives are uncontended and
  resolve in the local L1.)

Six runnable states fall out (three base configs, each with the I-cache on or off), MMU
and D-cache on in all of them:

| cores  | MMU | D-cache | L2     | SMP/SCU | I-cache |
|--------|-----|---------|--------|---------|---------|
| dual   | on  | on      | on     | on      | on/off  |
| single | on  | on      | on/off | off     | on/off  |

The SMP-only parts of the battery (the load-balance spread test, and the cross-core halves
of EDF and the mutex/IPI micro-benchmarks) compile only when `ENABLE_SMP=1`. Otherwise
`NUM_CPUS` collapses to 1 and the balancer and per-core loops fold onto CPU0. The host
harness keys each test on ELF-symbol presence, so a compiled-out test is simply skipped.

## Running the tests

There are two ways to get a binary onto the chip. The JTAG flow is faster for iteration.
The self-boot flow proves the thing actually boots from an SD card.

### JTAG-loaded (default, no SD card)

The driver starts OpenOCD, loads the ELF straight into memory over JTAG, releases the
core, collects telemetry, and writes figures and CSVs to a timestamped folder under
`test_results/`.

```bash
make build/test.elf
python3 test.py
# or, equivalently, build + run in one step:
make test
```

FreeRTOS battery:

```bash
make FREERTOS=1 build/test.elf
python3 test_freertos.py
```

### Self-boot from microSD

`make boot` rebuilds with the boot gate, wraps the OCRAM image as a preloader, and writes
it to the card's `0xA2` partition (it locates the card safely and writes only that slice,
no reformat). Use `FLASH_DRYRUN=1` to detect the card and stop before writing.

```bash
make boot                    # Kairos battery
make boot FREERTOS=1         # FreeRTOS battery
make boot FLASH_DRYRUN=1     # detect the card, write nothing
```

`make boot` writes both the preloader image and `build/app.bin` (the SDRAM code blob) to
the `0xA2` partition. `app.bin` lands `APP_OFFSET_SECTORS` sectors in (256 KiB), where
[`load_from_sd`](bsp/sd.c) reads it back at boot.

Then put the card in the board, power-cycle it, and attach:

```bash
python3 test.py --bootable          # Kairos
python3 test_freertos.py --bootable # FreeRTOS
```

In `--bootable` mode the board self-boots and spins at the boot gate. The driver attaches
over JTAG and releases it, rather than loading the ELF itself. Run `make clean` afterwards
so the gate isn't left compiled into a later `make test`.

## Results

Each run writes to `test_results/<timestamp>[-freertos]/`:

- `*.csv`: the raw and summary telemetry
- `*.png`: rendered figures (allocator sweeps, EDF schedule, context-switch and IRQ
  latency distributions, scheduler-cost-vs-task-count, and so on)

Curated runs used in the writeup are kept under `benchmarks/`, and the figures referenced
by `writeup.md` live in `figures/`.

## Repository layout

```
kernel/        scheduler, threads, kernel.s, GIC/timer/PMU/MMU, TLSF allocator
bsp/           board support: SDRAM PHY bring-up, SD, caches (Cyclone V specific)
linker/        linker scripts (OCRAM/SDRAM placement)
bench/         on-target workloads + telemetry hooks (ktrace)
freertos_port/ vendored FreeRTOS-Kernel + the DE1 glue for the FreeRTOS battery
openocd/       OpenOCD config + USB-Blaster II firmware
scripts/       SD flashing + figure renderers
test.py        Kairos battery driver
test_freertos.py  FreeRTOS battery driver
writeup.md     the design writeup
```

## Other targets

```bash
make lint                    # gcc -Wall -Wextra ... + cppcheck over the sources
make BOARD=qemu build/test.elf   # build for QEMU vexpress-a9 instead of real HW
```

## License

The vendored FreeRTOS kernel under `freertos_port/FreeRTOS-Kernel/` is MIT (see its
`LICENSE.md`). Portions of the BSP derive from Intel/Altera and U-Boot sources. For the
rest, see the repository.
