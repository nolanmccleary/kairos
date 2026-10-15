# FreeRTOS port plan (head-to-head against the Kairos kernel)

## Goal
Bring FreeRTOS scheduler + allocator onto the DE1-SoC (Cyclone V, Cortex-A9) next to
the existing kernel, reusing the current boot code unchanged, so we can measure
FreeRTOS vs Kairos on identical silicon, clock, and memory. Selectable at build time.

## Decisions (locked)
- **Scheduler comparisons are single core only.** The stable FreeRTOS CA9 port is
  single core. CPU1 parks in `cpu1_main` (WFI). For apples-to-apples scheduler rows,
  Kairos is also run single core. Dual-core EDF throughput is a separate axis that
  does not map onto FreeRTOS.
- **Build switch** `KERNEL ?= mine | freertos` picks which scheduler compiles. Only
  one scheduler is ever linked, so OCRAM stays in budget (FreeRTOS core at -Os is
  roughly 10-15 KB, replacing preempt_sched/thread/min_heap/thread_fifo).
- **Boot code stays as-is** except four small `#if USE_FREERTOS` guards (see below).
- **Allocator comparison does NOT need the switch.** Both allocators are just
  libraries, so heap_4 coexists with TLSF in one build. Run allocbench (TLSF) and the
  FreeRTOS alloc bench back to back, compare on the same run. This is step 1.

## Layout
```
freertos_port/
  PLAN.md                      (this file)
  FreeRTOSConfig.h             (authored, DE1-tuned)
  freertos_heap_shim.c         (authored: ucHeap in SDRAM + vTaskSuspendAll/xTaskResumeAll stubs)
  freertos_glue.c              (step 2: timer/tick/GIC/hooks)
  freertos_main.c              (step 2: creates tasks, vTaskStartScheduler)
  FreeRTOS-Kernel/             (vendored, MIT, verbatim)
    tasks.c list.c queue.c     (step 2)
    include/*                  (full header tree)
    portable/GCC/ARM_CA9/      (port.c portASM.S portmacro.h)
    portable/MemMang/heap_4.c  (step 1 allocator under test)
    LICENSE.md
```

## Build switch mechanics
- `KERNEL=freertos`: add vendored core + port + glue to objects, add `-DUSE_FREERTOS=1`
  and `-Wa,--defsym,USE_FREERTOS=1`, add the include dirs, drop
  preempt_sched.c/thread.c/min_heap.c/thread_fifo.c from CORE.
- `KERNEL=mine`: today's build, `USE_FREERTOS=0`.
- Allocator bench is gated separately by `BENCH_FREERTOS_ALLOC ?= 0`, additive to the
  normal build (both allocators present).

## Boot touch points (minimal, all `#if USE_FREERTOS`)
1. `bsp.c cpu0_startup()`: skip the trailing `psched_init()` and the CPU1 SMP engage.
2. `main.c`: dispatch to `freertos_main()` instead of the current body.
3. `startup.s _vectors`: swap `B _irq_handler`->`FreeRTOS_IRQ_Handler`,
   `B _swi_handler`->`FreeRTOS_SWI_Handler`.
4. `timers.c`/`gic.c`: do not arm the gtimer for the Kairos tick (FreeRTOS arms it).

## DE1 config facts (for FreeRTOSConfig.h / glue)
- GIC: PERIPHBASE 0xFFFEC000, GICC 0xFFFEC100, GICD 0xFFFED000, gtimer 0xFFFEC200.
- MPU clock 800 MHz, global timer at MPU/4 = 200 MHz, AUTOINC 199999 -> 1 kHz tick.
- gtimer clear is `GTIMER_ISR = 1` (same as c_irq_handler does today).
- c_irq_handler's switch-on-IAR maps 1:1 to the port's vApplicationIRQHandler.

## STEP 1: allocator path (CURRENT)
Milestone A (de-risk toolchain + memory): compile heap_4.c in arm-none-eabi with our
FreeRTOSConfig.h, link pvPortMalloc/vPortFree against the shim, place `ucHeap` in SDRAM.
  - `configAPPLICATION_ALLOCATED_HEAP = 1`, shim defines `uint8_t ucHeap[...]` in a
    SDRAM NOLOAD section (do not double-allocate the 16 MB TLSF heap; a few MB is enough
    to characterize heap_4 cost).
  - Stub `vTaskSuspendAll`/`xTaskResumeAll` (counter inc/dec, matching FreeRTOS behavior
    when no scheduler runs) so the measured cost stays honest.
  - `configUSE_MALLOC_FAILED_HOOK = 0`, `configASSERT` -> trap, trace macros off.
Milestone B (the artifact): `bench/workload_freertos_alloc.c` mirrors allocbench's exact
ops/sizes/iteration counts but calls pvPortMalloc/vPortFree, PMU-timed, emitting into a
parallel host_shared block so test.py plots TLSF vs heap_4 side by side.

## STEP 2: scheduler path (later)
Vector swap + freertos_glue (vApplicationSetupTimerInterrupt reusing gtimer, the tick
handler + GIC dispatch) + freertos_main + CPU1 parking. Then PMU microbenchmarks
(context switch, IRQ latency, scheduler latency vs task count) for the head-to-head.

## Target artifacts (the 6-artifact head-to-head)
allocator (step 1), context switch, IRQ latency, scheduler-latency-vs-ntasks,
uncontended mutex, EDF-vs-fixed-priority single-core. Micros = mechanism, the sweep is
the headline.

## Risks
- FreeRTOS CA9 port does its own GIC priority/binary-point setup; let it own the tick
  IRQ config or confirm cpu0_gic_init does not fight it. Most likely bringup snag.
- Keep both kernels in the same memory type (OCRAM, cacheable) for fair numbers.
- heap_4 cost is dominated by free-list length (fragmentation), not total size; match
  the allocbench access pattern so the comparison is meaningful.

## Design update (allocator step): size sweep + one-job-each scripts
- Benchmark is **alloc/free cost vs payload size** (`bench/workload_allocsweep.c`), not
  the fixed-size allocbench. ONE allocator-agnostic file via include guards: `#if
  USE_FREERTOS` -> pvPortMalloc/vPortFree, else kMalloc/kFree. Same host-shared symbols
  (`g_allocsweep_*`) in both binaries.
- Build switch is `make FREERTOS=1` (sets `-DUSE_FREERTOS=1`, links heap_4+shim + the
  separate `linker/freertos_heap.ld` fragment). de1-soc.ld and the default build carry
  NO FreeRTOS references.
- Appcode switch (main.c, include-guarded): FreeRTOS binary runs ONLY the sweep; the
  unused Kairos workloads are dropped by --gc-sections.
- Two sweeps: `workload_allocsweep.c` (cost vs payload size) and `workload_fragsweep.c`
  (cost vs free-list length / fragmentation -- the one that matters: TLSF O(1) flat,
  heap_4 first-fit O(n) climbing). Fragsweep threads its setup blocks through their own
  payload, so zero OCRAM scratch. Both allocator-agnostic via the same include guards.
- Scripts: `test.py` plots its TLSF sweeps (`allocsweep.png`, `fragsweep.png`).
  `test_freertos.py` is separate, drives the FreeRTOS binary, plots heap_4
  (`allocsweep_heap4.png`, `fragsweep_heap4.png`). It reuses only the generic JTAG
  plumbing + sweep collector/plot (`collect_sweep`/`plot_sweep`/`ALLOC_SWEEPS`) from
  test.py; test.py has no FreeRTOS phase.
- Shim stubs (vTaskSuspendAll/xTaskResumeAll/critical) gate on `USE_FREERTOS_SCHED`
  (undefined now), so they are present for the allocator build and ceded to tasks.c later.

## Status
- [x] feasibility assessed, FreeRTOS V11.1.0 vendored, deps understood
- [x] step 1 milestone A: heap_4.c compiles in the arm-none-eabi (gcc-only) toolchain
      (needed freertos_port/libc_shim/{stdlib.h,string.h}; no newlib).
- [x] step 1 milestone B: FREERTOS=1 build links (heap_4 in SDRAM .freertos_code, ucHeap
      in .freertos_heap @0x1300000); workload_allocsweep + appcode switch; test.py TLSF
      sweep plot; separate test_freertos.py. Both binaries build; default build FreeRTOS-free.
- [x] bootable: `make boot FREERTOS=1` packages correctly. heap_4 lives in OCRAM (the
      appcode switch gc's the Kairos workloads, freeing room), so the preloader/app.bin
      split and load_from_sd all work unchanged. preloader ~28.7K (OCRAM-fit), app.bin
      ~1.7K (the allocsweep workload). ucHeap is runtime-only SDRAM (NOLOAD).
- [ ] step 1 on hardware: run normal (make boot) + FreeRTOS (make boot FREERTOS=1), drive
      with test.py / test_freertos.py, compare the two allocsweep plots. (app.bin still has
      to be placed on the card at SD_APPCODE_LBA, same pre-existing step as the normal boot.)
- [ ] step 2 scheduler port
