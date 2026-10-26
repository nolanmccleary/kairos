CROSS   := arm-none-eabi
CC      := $(CROSS)-gcc
OBJCOPY := $(CROSS)-objcopy
OBJDUMP := $(CROSS)-objdump
NM      := $(CROSS)-nm



BOARD     ?= cyclone_v
TEST      ?= 0
FREERTOS  ?= 0
BOOT_TEST ?= 0

ENABLE_MMU        ?= 1
ENABLE_DCACHE     ?= 1
ENABLE_ICACHE     ?= 1
ENABLE_L2         ?= 1
ENABLE_SMP        ?= 1
ENABLE_NULL_GUARD ?= 1
USE_FIQ           ?= 0

# The FreeRTOS battery is always a test image.
ifeq ($(FREERTOS),1)
TEST := 1
endif


# ---------------------------------------------------------------------------
# Board
# ---------------------------------------------------------------------------

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

# Core kernel image -- what ships; a test build appends the payload below.
CORE := \
	main.c \
	kernel/kernel.s \
	bsp/bsp.c \
	bsp/boot.c \
	bsp/sequencer.c \
	bsp/sd.c \
	kernel/memset.c \
	kernel/tlsf.c \
	kernel/sched.c \
	kernel/thread_fifo.c \
	kernel/min_heap.c \
	kernel/pmu.c \
	kernel/thread.c \
	kernel/cpu.c \
	kernel/gic.c \
	kernel/irq.c \
	kernel/lock.c \
	kernel/timers.c \
	kernel/fault.c \
	kernel/mutex.s

# Host breakpoints, the per-tick trace hook, the metrics region; test images only.
TEST_INFRA := bench/ktrace.c bench/telemetry.c

# Kairos benchmark/validation workloads (test.py drives them); Kairos battery only.
TEST_WORKLOADS := \
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
	bench/workload_tickirq.c \
	bench/workload_gantt.c

# FreeRTOS battery sources (FREERTOS=1).
FK := freertos_port/FreeRTOS-Kernel
FREERTOS_SRC := \
	$(FK)/portable/MemMang/heap_4.c \
	freertos_port/freertos_heap_shim.c \
	$(FK)/tasks.c \
	$(FK)/list.c \
	$(FK)/queue.c \
	$(FK)/portable/GCC/ARM_CA9/port.c \
	$(FK)/portable/GCC/ARM_CA9/portASM.S \
	freertos_port/freertos_glue.c \
	freertos_port/freertos_main.c \
	bench/workload_freertos_schedsweep.c

# Assemble the final source list. TEST adds the infra; the Kairos workloads go in only
# for the Kairos battery ($(TEST)$(FREERTOS) == 10); FreeRTOS brings its own payload.
ifeq ($(TEST),1)
CORE += $(TEST_INFRA)
endif
ifeq ($(TEST)$(FREERTOS),10)
CORE += $(TEST_WORKLOADS)
endif
ifeq ($(FREERTOS),1)
CORE += $(FREERTOS_SRC)
endif


# ---------------------------------------------------------------------------
# Compiler / linker
# ---------------------------------------------------------------------------

# Cache/MMU config: compiled in AND emitted as .elf symbols (LDFLAGS) so the host reads it.
CFLAGS := \
	-mcpu=cortex-a9 \
	-marm \
	-Oz \
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
	-DENABLE_MMU=$(ENABLE_MMU) \
	-DENABLE_DCACHE=$(ENABLE_DCACHE) \
	-DENABLE_ICACHE=$(ENABLE_ICACHE) \
	-DENABLE_L2=$(ENABLE_L2) \
	-DENABLE_SMP=$(ENABLE_SMP) \
	-DENABLE_NULL_GUARD=$(ENABLE_NULL_GUARD) \
	-DBOOT_TEST=$(BOOT_TEST) \
	-DUSE_FIQ=$(USE_FIQ) \
	-Wa,--defsym,ENABLE_SMP=$(ENABLE_SMP) \
	-Wa,--defsym,BOOT_TEST=$(BOOT_TEST) \
	-Wa,--defsym,USE_FIQ=$(USE_FIQ) \
	-Wa,--defsym,BOARD_GICC_BASE=$(BOARD_GICC_BASE) \
	-Wa,--defsym,BOARD_IS_QEMU=$(BOARD_IS_QEMU) \
	-Wa,--defsym,USE_FREERTOS=$(FREERTOS)

ifeq ($(TEST),1)
CFLAGS += -DMODE_TEST
endif

# FreeRTOS needs NEON (softfp), its own include tree, and a second heap linker fragment.
LDSCRIPT_FLAGS := -T $(LDSCRIPT)
ifeq ($(FREERTOS),1)
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

# Named objects (mirrored under build/) so the linker places text by object path.
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
# Targets
# ---------------------------------------------------------------------------

# Build the test image and drive the harness over JTAG.
test:
	$(MAKE) TEST=1 build/test.elf
	python3 test.py

# Self-boot: wrap the test image as a preloader and write it + app.bin to the SD's
# 0xA2 partition (scripts/flash_sd.sh). Power-cycle, then drive with test.py --bootable.
boot:
	rm -rf build
	$(MAKE) BOOT_TEST=1 TEST=1 build/test.elf
	$(OBJCOPY) -O binary -R .appcode -R .kern_sdram -R .sdram_bss build/test.elf build/preloader.bin
	$(OBJCOPY) -O binary -j .appcode -j .kern_sdram build/test.elf build/app.bin
	@echo "preloader.bin: $$(wc -c < build/preloader.bin) bytes (OCRAM; must fit the boot ROM budget)"
	@echo "app.bin:       $$(wc -c < build/app.bin) bytes (SDRAM app/test code; load_from_sd reads this from SD)"
	mkimage -T socfpgaimage -d build/preloader.bin build/preloader.img && echo "wrote build/preloader.img"
	FLASH_DRYRUN=$(FLASH_DRYRUN) bash scripts/flash_sd.sh build/preloader.img

clean:
	rm -rf build


# ---------------------------------------------------------------------------
# Lint: two analyzers over our sources; skips bsp/sequencer.c (vendor Altera calibration).
# ---------------------------------------------------------------------------

LINT_WARN := -Wall -Wextra -Wshadow -Wundef -Wpointer-arith
LINT_SRC  := $(filter-out bsp/sequencer.c,$(filter %.c,$(CORE) $(TEST_INFRA) $(TEST_WORKLOADS)))

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
