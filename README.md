# Kairos

A from-scratch, bootable real-time kernel for the ARM Cortex-A9, targeting the Terasic
DE1-SoC (Cyclone V). Dual-core SMP, a hybrid Earliest-Deadline-First + round-robin
scheduler, a TLSF allocator, and its own boot path (no U-Boot, no Linux) that runs from
on-chip RAM and boots off a microSD card.

This README gives a basic build and run overview. For more in-depth architectural details please see [`writeup.md`](writeup.md).




```bash
# Kairos battery, loaded over JTAG (no SD card needed)
make build/test.elf
python3 test.py            # starts OpenOCD, loads the ELF, runs, writes figures to test_results/

# FreeRTOS battery (head-to-head comparison), same flow
make FREERTOS=1 build/test.elf
python3 test_freertos.py
```

## What's in the box

There are two independent test "batteries," each with its own binary and its own driver
script:

| Battery  | Build                           | Driver              | Scheduler / allocator        |
|----------|----------------------------------|---------------------|------------------------------|
| Kairos   | `make build/test.elf`            | `python3 test.py`   | Kairos EDF+RR / TLSF         |
| FreeRTOS | `make FREERTOS=1 build/test.elf` | `python3 test_freertos.py` | FreeRTOS fixed-priority / heap_4 |

Both are built from the same source tree and run on the same chip, so the numbers are
directly comparable. The FreeRTOS battery vendors FreeRTOS-Kernel V11.1.0 (MIT).

## Prerequisites

### Hardware
- Terasic **DE1-SoC** (Cyclone V) board
- Its on-board **USB-Blaster II** JTAG (a USB cable to the host)
- A **microSD card** (only needed for the self-boot flow, not for JTAG-loaded runs)

### Software
- **ARM bare-metal toolchain**: `arm-none-eabi-gcc` (plus `objcopy`/`objdump`/`nm`). The
  build is GCC-only and does not use newlib.
- **OpenOCD** (drives the USB-Blaster II)
- **Python 3** with **matplotlib** (the drivers collect telemetry and render figures)
- **u-boot-tools** for `mkimage` (only needed for `make boot`)
- *(optional)* **cppcheck** for `make lint`

```bash
# macOS
brew install --cask gcc-arm-embedded
brew install open-ocd u-boot-tools
pip3 install matplotlib

# Debian/Ubuntu
sudo apt install gcc-arm-none-eabi openocd u-boot-tools python3-matplotlib
```

The OpenOCD config and USB-Blaster II firmware live in [`openocd/`](openocd/)
(`de1soc.cfg`); the test drivers launch OpenOCD against it for you. If the blaster is
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

## Running the tests

There are two ways to get a binary onto the chip. The JTAG flow is faster for iteration;
the self-boot flow proves the thing actually boots from an SD card.

### JTAG-loaded (default, no SD card)

The driver starts OpenOCD, loads the ELF straight into memory over JTAG, releases the
core, collects telemetry, and writes figures + CSVs to a timestamped folder under
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

> Note: `make boot` flashes the preloader but does **not** yet place `build/app.bin` on the
> card. Write `app.bin` to the card at `SD_APPCODE_LBA` (see [`bsp/sd.c`](bsp/sd.c)) as a
> separate step.

Then put the card in the board, power-cycle it, and attach:

```bash
python3 test.py --bootable          # Kairos
python3 test_freertos.py --bootable # FreeRTOS
```

In `--bootable` mode the board self-boots and spins at the boot gate; the driver attaches
over JTAG and releases it, rather than loading the ELF itself. Run `make clean` afterwards
so the gate isn't left compiled into a later `make test`.

## Results

Each run writes to `test_results/<timestamp>[-freertos]/`:

- `*.csv` — the raw/summary telemetry
- `*.png` — rendered figures (allocator sweeps, EDF schedule, context-switch and IRQ
  latency distributions, scheduler-cost-vs-task-count, etc.)

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
