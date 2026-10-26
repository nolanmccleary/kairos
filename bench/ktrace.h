#ifndef KTRACE_H
#define KTRACE_H


#ifdef MODE_TEST

#include <stdint.h>

#include "cpu.h"
#include "pmu.h"
#include "telemetry.h"
#include "thread.h"



/* -------------------------------------------------------------------------
 * OpenOCD hardware-breakpoint sites
 *
 * Python installs one hardware breakpoint at each function address.
 *
 * The address itself is the target -> host reason:
 *
 *     ktrace_bp_alloc_done  -> pre-EDF benchmark suite complete (alloc/rmw/matmul)
 *     ktrace_bp_edf_ready   -> current EDF trial configured
 *     ktrace_bp_edf_done    -> complete EDF sweep finished
 * ------------------------------------------------------------------------- */

void ktrace_bp_alloc_done(void);
void ktrace_bp_edf_ready(void);
void ktrace_bp_edf_done(void);
void ktrace_bp_balance_ready(void);
void ktrace_bp_balance_done(void);
void ktrace_bp_micro_done(void);
void ktrace_bp_suspend_done(void);
void ktrace_bp_persist_ready(void);
void ktrace_bp_persist_done(void);
void ktrace_bp_sched_sweep_done(void);
void ktrace_bp_tickirq_done(void);
void ktrace_bp_gantt_done(void);

/* Gantt: one event per tick-IRQ fire -- its x-index (= thread-trace index for that tick,
 * so the IRQ mark lines up with the thread lanes), core, and IRQ id. */
void ktrace_irq_fire(uint32_t core, uint32_t id, uint32_t cycles);
extern volatile uint32_t g_irq_ev_count;

/* Shared trace state (defined in ktrace.c): per-tick schedule-trace index + active flag. */
extern uint32_t trace_active;
extern volatile uint32_t g_trace_len[NUM_CPUS];


/* -------------------------------------------------------------------------
 * Microbenchmark hooks (test builds only), implemented in workload_micro.c:
 *   ktrace_sched_cost -- next_thread's per-invocation cost, classified by whether
 *                        a different thread actually ran (switch) or not (resume)
 *   ktrace_irq_ping   -- CPU_IRQ_PING ISR body: timestamps the SGI dispatch
 *   ktrace_mutex_hold -- CPU_MUTEX_HOLD ISR body: CPU1's contended-lock holder loop
 * ------------------------------------------------------------------------- */

void ktrace_sched_cost(uint32_t cost, int switched);
void ktrace_irq_ping(void);
void ktrace_mutex_hold(void);


/* -------------------------------------------------------------------------
 * Per-tick scheduler hook (test builds only)
 *
 * next_thread() calls this once per tick with the task about to run. The EDF
 * workload (workload_edf.c) implements it: records the Gantt trace and mirrors
 * the running task's metrics into the uncached host-readable region. Kernel is
 * blind to what it does -- pure test payload behind a MODE_TEST gate.
 * ------------------------------------------------------------------------- */

void ktrace_edf_tick(thread_t* running);

#define KTRACE_TICK_EXIT(running) \
    ktrace_edf_tick(running)


#define KTRACE_ALLOC_DONE() \
    ktrace_bp_alloc_done()

#define KTRACE_EDF_READY() \
    ktrace_bp_edf_ready()

#define KTRACE_EDF_DONE() \
    ktrace_bp_edf_done()

#define KTRACE_BALANCE_READY() \
    ktrace_bp_balance_ready()

#define KTRACE_BALANCE_DONE() \
    ktrace_bp_balance_done()

#define KTRACE_MICRO_DONE() \
    ktrace_bp_micro_done()

#define KTRACE_SUSPEND_DONE() \
    ktrace_bp_suspend_done()

#define KTRACE_PERSIST_READY() \
    ktrace_bp_persist_ready()

#define KTRACE_PERSIST_DONE() \
    ktrace_bp_persist_done()

#define KTRACE_SCHED_SWEEP_DONE() \
    ktrace_bp_sched_sweep_done()

#define KTRACE_TICKIRQ_DONE() \
    ktrace_bp_tickirq_done()

#define KTRACE_GANTT_DONE() \
    ktrace_bp_gantt_done()

#define KTRACE_IRQ_FIRE(core, id, cycles) \
    ktrace_irq_fire((core), (id), (cycles))

#define KTRACE_SCHED_COST(cost, switched) \
    ktrace_sched_cost((cost), (switched))

#define KTRACE_IRQ_PING() \
    ktrace_irq_ping()

#define KTRACE_MUTEX_HOLD() \
    ktrace_mutex_hold()


/* -------------------------------------------------------------------------
 * Host -> target EDF release
 *
 * This is actual target state rather than a breakpoint reason.
 *
 * Python leaves it zero while an EDF trial runs. After the measurement
 * window Python halts the target, samples state, writes 1, and resumes.
 * ------------------------------------------------------------------------- */

extern volatile uint32_t g_test_release;


void ktrace_wait_release(void);


#define KTRACE_WAIT_RELEASE() \
    ktrace_wait_release()


#define KTRACE_RELEASE_PENDING() \
    (g_test_release != 0u)


/* -------------------------------------------------------------------------
 * Self-boot gate (BOOT_TEST builds only)
 *
 * When the board self-boots from SD there is no debugger present at power-on,
 * so an OCD-programmed breakpoint can't stop it before the tests. Instead the
 * firmware spins on g_boot_release (zeroed on entry) at the very top of main(),
 * BEFORE c_startup(). Attach JTAG afterwards and release it by writing the flag
 * (test.py --bootable). g_boot_release lives in uncached .bss, so the JTAG
 * write is coherent with the CPU's spin.
 * ------------------------------------------------------------------------- */

#if BOOT_TEST

extern volatile uint32_t g_boot_release;

void ktrace_wait_boot(void);

#define KTRACE_WAIT_BOOT() \
    ktrace_wait_boot()

#else

#define KTRACE_WAIT_BOOT() ((void)0)

#endif


#else



#define KTRACE_ALLOC_DONE()          ((void)0)
#define KTRACE_EDF_READY()           ((void)0)
#define KTRACE_EDF_DONE()            ((void)0)
#define KTRACE_BALANCE_READY()       ((void)0)
#define KTRACE_BALANCE_DONE()        ((void)0)
#define KTRACE_MICRO_DONE()          ((void)0)
#define KTRACE_SUSPEND_DONE()        ((void)0)
#define KTRACE_PERSIST_READY()       ((void)0)
#define KTRACE_PERSIST_DONE()        ((void)0)
#define KTRACE_SCHED_SWEEP_DONE()    ((void)0)
#define KTRACE_SCHED_COST(cost, switched) ((void)0)
#define KTRACE_IRQ_PING()            ((void)0)
#define KTRACE_MUTEX_HOLD()          ((void)0)

#define KTRACE_TICK_EXIT(running)    ((void)0)

#define KTRACE_WAIT_RELEASE()        ((void)0)
#define KTRACE_RELEASE_PENDING()     (0)

#define KTRACE_WAIT_BOOT()           ((void)0)

#define KTRACE_IRQ_FIRE(core, id, cycles) ((void)0) 

#endif

#endif
