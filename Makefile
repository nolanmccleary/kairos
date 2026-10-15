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
	bench/workload_persistent.c \
	bench/workload_allocsweep.c \
	bench/workload_fragsweep.c \
	bench/workload_schedsweep.c \
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

FREERTOS ?= 0   # 0 = Kairos battery (test.py), 1 = FreeRTOS battery (test_freertos.py)


# Self-boot test gate: compile the spin at the top of main() that hangs until
# JTAG writes g_boot_release. Needed only for the no-debugger SD boot flow --
# enable with `make boot`, drive with `python test.py --bootable`.
BOOT_TEST     ?= 0

CFLAGS := \
	-mcpu=cortex-a9 \
	-marm \
	-Os \
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
	-Wa,--defsym,BOARD_IS_QEMU=$(BOARD_IS_QEMU) \
	-Wa,--defsym,USE_FREERTOS=$(FREERTOS)


# FreeRTOS battery (make FREERTOS=1). The ONE FreeRTOS binary: FreeRTOS scheduler + heap_4,
# with every bench (allocator sweeps + the task-stacking scheduler sweep) run as FreeRTOS
# tasks. FREERTOS=0 is the Kairos battery; the default build and de1-soc.ld carry no
# FreeRTOS references. workload_allocsweep.c/workload_fragsweep.c are allocator-agnostic
# (USE_FREERTOS -> pvPortMalloc/vPortFree) and shared with the Kairos battery.
LDSCRIPT_FLAGS := -T $(LDSCRIPT)
ifeq ($(FREERTOS),1)
  FK := freertos_port/FreeRTOS-Kernel
  CORE += $(FK)/portable/MemMang/heap_4.c freertos_port/freertos_heap_shim.c \
          $(FK)/tasks.c $(FK)/list.c $(FK)/queue.c \
          $(FK)/portable/GCC/ARM_CA9/port.c \
          $(FK)/portable/GCC/ARM_CA9/portASM.S \
          freertos_port/freertos_glue.c freertos_port/freertos_main.c \
          bench/workload_freertos_schedsweep.c
  CFLAGS += -DUSE_FREERTOS=1 -mfpu=neon -mfloat-abi=softfp \
            -Ifreertos_port/libc_shim -Ifreertos_port \
            -Ifreertos_port/FreeRTOS-Kernel/include \
            -Ifreertos_port/FreeRTOS-Kernel/portable/GCC/ARM_CA9
  LDSCRIPT_FLAGS := -Wl,-dT,$(LDSCRIPT) -Wl,-T,linker/freertos_heap.ld
endif


LDFLAGS := \
	$(LDSCRIPT_FLAGS) \
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

# Compile to named objects (mirrored under build/) so the linker can place text
# by object-file path -- bench/workload_*.o -> SDRAM .appcode, everything else OCRAM.
OBJ := $(patsubst %,build/%.o,$(basename $(CORE)))

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/test.elf: $(OBJ) $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBGCC)
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
	rm -rf build
	$(MAKE) BOOT_TEST=1 build/test.elf
	$(OBJCOPY) -O binary -R .appcode build/test.elf build/preloader.bin
	$(OBJCOPY) -O binary -j .appcode build/test.elf build/app.bin
	@echo "preloader.bin: $$(wc -c < build/preloader.bin) bytes (OCRAM; must fit the boot ROM budget)"
	@echo "app.bin:       $$(wc -c < build/app.bin) bytes (SDRAM app/test code; load_from_sd reads this from SD)"
	mkimage -T socfpgaimage -d build/preloader.bin build/preloader.img && echo "wrote build/preloader.img"
	FLASH_DRYRUN=$(FLASH_DRYRUN) bash scripts/flash_sd.sh build/preloader.img
	@echo "NOTE: app.bin must still be written to the card at SD_APPCODE_LBA (see bsp/sd.c) -- flash_sd.sh does not place it yet."


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
