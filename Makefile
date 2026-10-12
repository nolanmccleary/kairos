CROSS   := arm-none-eabi

CC      := $(CROSS)-gcc
OBJCOPY := $(CROSS)-objcopy
OBJDUMP := $(CROSS)-objdump
NM      := $(CROSS)-nm

# Board selection: cyclone_v (DE1-SoC, real HW) | qemu (vexpress-a9, emulated).
# MODE_TEST is orthogonal -- any board builds with or without the test payload.
BOARD ?= cyclone_v

ifeq ($(BOARD),cyclone_v)
  BOARD_ID        := BOARD_CYCLONE_V
  LDSCRIPT        := linker/de1-soc.ld
  BOARD_CDEFS     := -DBOARD_DE1_SOC
  BOARD_GICC_BASE := 0xFFFEC100
  BOARD_IS_QEMU   := 0
else ifeq ($(BOARD),qemu)
  BOARD_ID        := BOARD_QEMU
  LDSCRIPT        := linker/qemu.ld
  BOARD_CDEFS     :=
  BOARD_GICC_BASE := 0x1E000100
  BOARD_IS_QEMU   := 1
else
  $(error unknown BOARD '$(BOARD)' (expected cyclone_v or qemu))
endif



# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

CORE := \
	main.c \
	kernel/startup.s \
	bsp/bsp.c \
	bsp/boot.c \
	bsp/sequencer.c \
	bsp/sd.c \
	kernel/tlsf.c \
	kernel/preempt_sched.c \
	kernel/thread_fifo.c \
	kernel/min_heap.c \
	kernel/pmu.c \
	kernel/thread.c \
	kernel/cpu.c \
	kernel/gic.c \
	kernel/irq.c \
	kernel/lock.c \
	kernel/timers.c \
	bench/ktrace.c \
	kernel/fault.c \
	bench/telemetry.c \
	bench/workload_edf.c \
	bench/workload_allocbench.c \
	bench/workload_rmw.c \
	bench/workload_matmul.c \
	bench/workload_micro.c \
	bench/workload_suspend.c \
	kernel/mutex.s \


# ---------------------------------------------------------------------------
# Compiler / linker
# ---------------------------------------------------------------------------

# Cache/MMU enables -- single source of truth: compiled into the code AND emitted
# as absolute symbols (LDFLAGS below) so the host reads the config straight from
# the .elf symbol table, without any runtime field or loaded-image inflation.
ENABLE_MMU    ?= 1
ENABLE_DCACHE ?= 1
ENABLE_ICACHE ?= 1
ENABLE_L2     ?= 1   # PL310 outer (L2) cache; requires ENABLE_MMU
ENABLE_SMP    ?= 1
ENABLE_NULL_GUARD ?= 1   # unmap VA 0..1MB so NULL / near-NULL derefs fault (needs ENABLE_MMU)


# Self-boot test gate: compile the spin at the top of main() that hangs until
# JTAG writes g_boot_release. Needed only for the no-debugger SD boot flow --
# enable with `make boot`, drive with `python test.py --bootable`.
BOOT_TEST     ?= 0

CFLAGS := \
	-mcpu=cortex-a9 \
	-marm \
	-O1 \
	-g \
	-ffreestanding \
	-nostdlib \
	-ffunction-sections \
	-fdata-sections \
	-I. \
	-Ibsp \
	-Ikernel \
	-Ibench \
	$(BOARD_CDEFS) \
	-DBOARD=$(BOARD_ID) \
	-DMODE_TEST \
	-DENABLE_MMU=$(ENABLE_MMU) \
	-DENABLE_DCACHE=$(ENABLE_DCACHE) \
	-DENABLE_ICACHE=$(ENABLE_ICACHE) \
	-DENABLE_L2=$(ENABLE_L2) \
	-DENABLE_SMP=$(ENABLE_SMP) \
	-DENABLE_NULL_GUARD=$(ENABLE_NULL_GUARD) \
	-DBOOT_TEST=$(BOOT_TEST) \
	-Wa,--defsym,ENABLE_SMP=$(ENABLE_SMP) \
	-Wa,--defsym,BOOT_TEST=$(BOOT_TEST) \
	-Wa,--defsym,MODE_TEST=1 \
	-Wa,--defsym,BOARD_GICC_BASE=$(BOARD_GICC_BASE) \
	-Wa,--defsym,BOARD_IS_QEMU=$(BOARD_IS_QEMU)


LDFLAGS := \
	-T $(LDSCRIPT) \
	-Wl,--build-id=none \
	-Wl,--gc-sections \
	-Wl,-Map=build/test.map \
	-Wl,--defsym=_cfg_enable_mmu=$(ENABLE_MMU) \
	-Wl,--defsym=_cfg_enable_dcache=$(ENABLE_DCACHE) \
	-Wl,--defsym=_cfg_enable_icache=$(ENABLE_ICACHE) \
	-Wl,--defsym=_cfg_enable_l2=$(ENABLE_L2) \
	-Wl,--defsym=_cfg_enable_smp=$(ENABLE_SMP)


LIBGCC := $(shell $(CC) -mcpu=cortex-a9 -marm -print-libgcc-file-name)


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

build/test.elf: $(CORE) $(LDSCRIPT)
	mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CORE) $(LIBGCC)
	$(OBJDUMP) -d $@ > build/test.dis


# ---------------------------------------------------------------------------
# Test: build + drive the harness over JTAG (test.py loads the image + runs it)
# ---------------------------------------------------------------------------

test: build/test.elf
	python3 test.py


# ---------------------------------------------------------------------------
# Self-boot: the ONE image + the BOOT_TEST gate, wrapped as a preloader and
# written to the microSD's 0xA2 partition (scripts/flash_sd.sh locates the card
# safely and writes only that slice -- no reformat). Power-cycle, then drive with
# `python test.py --bootable`. Force-rebuilds test.elf (only a -D flag changes,
# which make can't see in timestamps); `make clean` afterwards before a normal
# `make test` so the gate isn't left compiled in. `make boot FLASH_DRYRUN=1`
# detects the card and stops before writing.
# ---------------------------------------------------------------------------

boot:
	rm -f build/test.elf
	$(MAKE) BOOT_TEST=1 build/test.elf
	$(OBJCOPY) -O binary build/test.elf build/preloader.bin
	@echo "preloader.bin: $$(wc -c < build/preloader.bin) bytes (must fit the boot ROM OCRAM budget)"
	mkimage -T socfpgaimage -d build/preloader.bin build/preloader.img && echo "wrote build/preloader.img"
	FLASH_DRYRUN=$(FLASH_DRYRUN) bash scripts/flash_sd.sh build/preloader.img


clean:
	rm -rf build


# Lint = two analyzers over our sources (warnings are off in the normal build).
# Skips bsp/sequencer.c (vendor-generated Altera SDRAM calibration; its sign-compare/
# unused noise isn't ours to fix). Both track the ENABLE_* config, so e.g.
# `make ENABLE_SMP=0 lint` lints that variant.
LINT_WARN := -Wall -Wextra -Wshadow -Wundef -Wpointer-arith
LINT_SRC  := $(filter-out bsp/sequencer.c,$(filter %.c,$(CORE)))

LINT_DEFS := $(BOARD_CDEFS) -DBOARD=$(BOARD_ID) -DMODE_TEST -DENABLE_MMU=$(ENABLE_MMU) \
	-DENABLE_DCACHE=$(ENABLE_DCACHE) -DENABLE_ICACHE=$(ENABLE_ICACHE) \
	-DENABLE_L2=$(ENABLE_L2) -DENABLE_SMP=$(ENABLE_SMP) -DBOOT_TEST=$(BOOT_TEST)

# unix32: 32-bit long + pointer, matching the ARM32 EABI cppcheck can't otherwise know.
CPPCHECK := cppcheck --quiet --enable=warning,style,performance,portability \
	--std=c11 --platform=unix32 --inline-suppr --suppress=missingIncludeSystem \
	-I. -Ibsp -Ikernel -Ibench $(LINT_DEFS)

lint:
	@echo "=== gcc ($(LINT_WARN)) ==="
	-@$(CC) $(CFLAGS) $(LINT_WARN) -fsyntax-only $(LINT_SRC)
	@echo "=== cppcheck ==="
	-@$(CPPCHECK) $(LINT_SRC)
	@echo "=== lint done ==="


.PHONY: test boot clean lint
