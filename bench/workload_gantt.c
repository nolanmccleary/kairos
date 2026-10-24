/* Gantt component: a realistic mixed run -- EDF periodics + a persistent thread + tick-IRQs
 * all live at once on CPU0 -- traced per tick. The per-tick thread trace (g_sched_trace,
 * reused from workload_edf.c) gives the thread lanes; ktrace_irq_fire (called from run_irqs)
 * marks each IRQ fire with its run cost on the same tick axis. Like the EDF/persistent
 * tests, the window is bounded by the host: main parks in KTRACE_WAIT_RELEASE() (the
 * scheduler force-switches it back in when the host sets g_test_release), so a for(;;)
 * persistent hogging the slack can't deadlock teardown. Thread ids / periods here must
 * match the GANTT_* constants in test.py. */

#include <stdint.h>

#include "cpu.h"
#include "irq.h"
#include "ktrace.h"
#include "pmu.h"
#include "sched.h"
#include "telemetry.h"
#include "thread.h"
#include "tlsf.h"


#define GANTT_N_EDF  3u
#define GANTT_N_IRQ  2u

/* thread ids: EDF 0,1,2  persistent 3. IRQ ids 0,1 live in their own (mark) lane. */
static const uint32_t gantt_edf_periods[GANTT_N_EDF] = { 6u, 10u, 15u };
static const uint32_t gantt_irq_periods[GANTT_N_IRQ] = { 5u, 13u };


/* Trace infra reused from workload_edf.c. */
extern uint32_t trace_active;
extern volatile uint32_t g_trace_len[NUM_CPUS];


/* EDF periodic job: a little work then returns, so each release finishes and re-arms. */
static sys_exit_e gantt_busy(void) { for (volatile uint32_t i = 0; i < 100000u; i++) { } return SYS_OK; }

/* Persistent job: never returns -- the scheduler slices it in to fill slack. */
static sys_exit_e gantt_persist(void) { for (;;) { for (volatile uint32_t i = 0; i < 50000u; i++) { } } }

/* IRQ job does nothing; the fire itself (+ its cycle cost) is what the Gantt marks. */
static sys_exit_e gantt_irq_job(void) { return SYS_OK; }


static irq_t gantt_irq_pool[GANTT_N_IRQ];


void gantt_run(void)
{
    pmu_init();
    heap_init();
    psched_init();

    thread_t* edf[GANTT_N_EDF];
    thread_t* persist;

    g_test_release = 0u;

    /* Construction must not race the scheduler. */
    __asm__ volatile ("cpsid " PREEMPT_MASK ::: "memory");

    for (uint32_t c = 0; c < NUM_CPUS; c++) g_trace_len[c] = 0u;
    g_irq_ev_count = 0u;
    trace_active = 1u;

    for (uint32_t i = 0; i < GANTT_N_EDF; i++)
    {
        edf[i] = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, gantt_busy,
                                             gantt_edf_periods[i], PERIODIC, i);
    }
    persist = add_persistent_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, gantt_persist,
                                            3u, LOW_PRIORITY);
    for (uint32_t i = 0; i < GANTT_N_IRQ; i++)
    {
        mount_tick_irq_to_core(&gantt_irq_pool[i], CPU0, gantt_irq_job, gantt_irq_periods[i], i);
    }

    __asm__ volatile ("dmb sy" ::: "memory");
    __asm__ volatile ("cpsie " PREEMPT_MASK ::: "memory");

    /* Host samples the trace after a short window, then releases; the scheduler forces
     * main back in so we can tear down even while the persistent hogs the slack. */
    KTRACE_WAIT_RELEASE();

    __asm__ volatile ("cpsid " PREEMPT_MASK ::: "memory");
    trace_active = 0u;
    for (uint32_t i = 0; i < GANTT_N_EDF; i++) kill_thread(edf[i]);
    kill_thread(persist);
    for (uint32_t i = 0; i < GANTT_N_IRQ; i++) unmount_tick_irq(&gantt_irq_pool[i]);
    __asm__ volatile ("cpsie " PREEMPT_MASK ::: "memory");

    psched_deinit();

    for (uint32_t i = 0; i < GANTT_N_EDF; i++) if (edf[i]) kFree(edf[i]);
    if (persist) kFree(persist);

    KTRACE_GANTT_DONE();
}
