/* Tick-IRQ component: mount several periodic tick-IRQs (these run directly from the
 * scheduler tick in run_irqs, not as threads), let them fire for a fixed window of ticks,
 * then check each fired count ~= elapsed_ticks / period. Also exercises mount + unmount.
 * Single core (CPU0): all IRQs land in CPU0's irqHeap, so only CPU0's tick drives them. */

#include <stdint.h>

#include "cpu.h"
#include "irq.h"
#include "ktrace.h"
#include "pmu.h"
#include "sched.h"
#include "telemetry.h"
#include "thread.h"
#include "tlsf.h"


#define TICKIRQ_N          4u
#define TICKIRQ_RUN_TICKS  500u

/* Distinct periods (ticks) so each IRQ fires at its own rate; count should land near
 * RUN_TICKS/period: ~{50, 25, 20, 10}. */
static const uint32_t tickirq_periods[TICKIRQ_N] = { 10u, 20u, 25u, 50u };


/* Host-readable results. test.py checks count[i] ~= run_ticks / period[i]. */
HOST_SHARED volatile uint32_t g_tickirq_count[TICKIRQ_N];
HOST_SHARED uint32_t          g_tickirq_periods[TICKIRQ_N];
HOST_SHARED uint32_t          g_tickirq_run_ticks;
HOST_SHARED uint32_t          g_tickirq_n;


/* One callback per IRQ -- the func takes no args, so each bumps its own counter. */
static sys_exit_e tickirq_job_0(void) { g_tickirq_count[0]++; return SYS_OK; }
static sys_exit_e tickirq_job_1(void) { g_tickirq_count[1]++; return SYS_OK; }
static sys_exit_e tickirq_job_2(void) { g_tickirq_count[2]++; return SYS_OK; }
static sys_exit_e tickirq_job_3(void) { g_tickirq_count[3]++; return SYS_OK; }

static sys_exit_e (*const TICKIRQ_JOBS[TICKIRQ_N])(void) =
{
    tickirq_job_0, tickirq_job_1, tickirq_job_2, tickirq_job_3,
};


/* Caller-owned IRQ storage (irq_t carries no stack, so a small static pool is fine). */
static irq_t tickirq_pool[TICKIRQ_N];


void tickirq_run(void)
{
    pmu_init();
    heap_init();
    psched_init();
    __asm__ volatile ("cpsie " PREEMPT_MASK ::: "memory");

    g_tickirq_n         = TICKIRQ_N;
    g_tickirq_run_ticks = TICKIRQ_RUN_TICKS;
    for (uint32_t i = 0; i < TICKIRQ_N; i++)
    {
        g_tickirq_count[i]   = 0u;
        g_tickirq_periods[i] = tickirq_periods[i];
    }

    __asm__ volatile ("cpsid " PREEMPT_MASK ::: "memory");
    for (uint32_t i = 0; i < TICKIRQ_N; i++)
    {
        mount_tick_irq_to_core(&tickirq_pool[i], CPU0, TICKIRQ_JOBS[i], tickirq_periods[i], i);
    }
    __asm__ volatile ("cpsie " PREEMPT_MASK ::: "memory");

    /* Let the scheduler tick drive run_irqs for a fixed window. */
    uint32_t t0 = g_cpus[CPU0].ticks;
    while ((g_cpus[CPU0].ticks - t0) < TICKIRQ_RUN_TICKS)
    {
        __asm__ volatile ("" ::: "memory");
    }

    __asm__ volatile ("cpsid " PREEMPT_MASK ::: "memory");
    for (uint32_t i = 0; i < TICKIRQ_N; i++)
    {
        unmount_tick_irq(&tickirq_pool[i]);
    }
    __asm__ volatile ("cpsie " PREEMPT_MASK ::: "memory");

    psched_deinit();

    KTRACE_TICKIRQ_DONE();
}
