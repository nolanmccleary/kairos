#include <stdint.h>

#include "telemetry.h"
#include "ktrace.h"
#include "cpu.h"
#include "gic.h"
#include "irq.h"
#include "lock.h"
#include "thread.h"
#include "tlsf.h"
#include "sched.h"


#ifdef MODE_TEST


/* -------------------------------------------------------------------------
 * Primitive-cost microbenchmarks, each a self-contained phase that leaves the
 * machine in a predictable state. The host reads the raw per-sample cycle counts
 * and builds the distributions (min/mean/p99/max + histogram). The whole file is
 * MODE_TEST-only: none of these host-readable regions exist in a production image.
 *
 *   ctxsw  -- next_thread() cost per tick, split by outcome into "switch" (a
 *             different thread ran) and "resume" (same thread continued). The
 *             two are workload-invariant; the switch:resume ratio p is not, so
 *             we also report the counts that give p for this fixed taskset.
 *   irqlat -- SGI dispatch latency: trigger -> ISR body, same core, same PMU.
 *   mutex  -- lock+unlock uncontended (one core) and acquire latency contended
 *             (CPU1 holds/releases in a tight loop while CPU0 measures).
 *   memlat -- uncached single-word store latency, SDRAM vs OCRAM.
 * ------------------------------------------------------------------------- */

#define CTXSW_SAMPLES    1024u
#define IRQ_SAMPLES      1024u
#define MUTEX_SAMPLES    1024u
#define MEMLAT_SAMPLES   8192u

#define CTX_WORK_ITERS   8000u    /* per-release busy work for the ctxsw taskset (sub-tick) */
#define MUTEX_HOLD_WORK  100u     /* CPU1's critical-section length between lock and unlock */
#define MUTEX_BACKOFF    400u     /* CPU1 stays out of the lock this long after unlock, so a
                                     wfe-fair CPU0 isn't starved by CPU1 immediately re-locking */
#define MUTEX_CON_CAP    200000u  /* CPU0 contended-acquire retry cap: bounds the wait, never hangs */
#define MUTEX_SPINUP     100000u  /* let CPU1 enter its holder loop before timing */
#define CTXSW_BUDGET     1000000000u /* hard spin cap: ~seconds, so a stall can't hang forever */


/* --- ctx-switch ------------------------------------------------------------ */

HOST_SHARED uint32_t g_ctxsw_switch[CTXSW_SAMPLES];
HOST_SHARED uint32_t g_ctxsw_resume[CTXSW_SAMPLES];
HOST_SHARED volatile uint32_t g_ctxsw_switch_count;   /* unbounded; samples capped at CTXSW_SAMPLES */
HOST_SHARED volatile uint32_t g_ctxsw_resume_count;

static volatile uint32_t ctxsw_active;


/* Called from next_thread() on every tick (both cores). Sample CPU0 only so the
 * two cores can't race the shared counters, and so the distribution is one core's. */
extern int schedsweep_sample(uint32_t cost);

void ktrace_sched_cost(uint32_t cost, int switched)
{
    if (schedsweep_sample(cost)) return;   /* schedsweep phase captures the per-tick cost */

    if (!ctxsw_active || curr_core() != CPU0) return;

    if (switched)
    {
        if (g_ctxsw_switch_count < CTXSW_SAMPLES) g_ctxsw_switch[g_ctxsw_switch_count] = cost;
        g_ctxsw_switch_count++;
    }
    else
    {
        if (g_ctxsw_resume_count < CTXSW_SAMPLES) g_ctxsw_resume[g_ctxsw_resume_count] = cost;
        g_ctxsw_resume_count++;
    }
}


static volatile uint32_t ctx_sink;

static sys_exit_e ctx_job(void)
{
    uint32_t x = 0;
    for (uint32_t i = 0; i < CTX_WORK_ITERS; i++) x += i;
    ctx_sink = x;
    return SYS_OK;
}


static void ctxsw_run(void)
{
    pmu_init();
    heap_init();
    psched_init();

    g_ctxsw_switch_count = 0u;
    g_ctxsw_resume_count = 0u;

    thread_t* h0;
    thread_t* h1;
    thread_t* h2;

    /* Short periods (ticks) so a task is released on ~half of all ticks -> switch
     * ticks accrue fast enough to fill the ring in seconds, not ~half a minute. */
    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");
    h0 = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, ctx_job, 4u,  PERIODIC, 0u);
    h1 = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, ctx_job, 6u,  PERIODIC, 1u);
    h2 = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, ctx_job, 10u, PERIODIC, 2u);
    ctxsw_active = 1u;
    __asm__ __volatile__("dmb sy" ::: "memory");
    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    /* Spin on main while ticks fire; the next_thread hook fills both rings. main
     * being runnable is what generates the resume (main->main) ticks, and the
     * two periodic jobs generate the switch ticks. */
    uint32_t budget = 0u;
    while ((g_ctxsw_switch_count < CTXSW_SAMPLES || g_ctxsw_resume_count < CTXSW_SAMPLES)
           && budget < CTXSW_BUDGET)
    {
        budget++;
        __asm__ __volatile__("" ::: "memory");
    }

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");
    ctxsw_active = 0u;
    kill_thread(h0);
    kill_thread(h1);
    kill_thread(h2);
    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    psched_deinit();

    if (h0) kFree(h0);
    if (h1) kFree(h1);
    if (h2) kFree(h2);
}


/* --- IRQ dispatch latency -------------------------------------------------- */

HOST_SHARED uint32_t g_irqlat[IRQ_SAMPLES];

static volatile uint32_t irq_ping_done;
static volatile uint32_t irq_ping_t1;


/* CPU_IRQ_PING ISR body -- timestamp as early as the handler reaches it. */
void ktrace_irq_ping(void)
{
    irq_ping_t1 = pmu_cycles();
    irq_ping_done = 1u;
}


static void irqlat_run(void)
{
    cpu_core_e core = curr_core();

    /* GIC CPU interface up. Mask this core's private-timer PPI (banked bit 27)
     * via GICD_ICENABLER0 so no scheduler tick preempts the measurement; the SGI
     * path (IDs 0-15) stays enabled regardless. */
    *(volatile uint32_t*)(GICD_BASE + 0x180u) = (1u << 27);
    GICD_CTLR = 1u;
    GICC_PMR  = 0xFFu;
    GICC_CTLR = 1u;

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    for (uint32_t i = 0; i < IRQ_SAMPLES; i++)
    {
        irq_ping_done = 0u;
        __asm__ __volatile__("dmb sy" ::: "memory");

        uint32_t t0 = pmu_cycles();
        GICD_SGIR = (1u << (16u + (uint32_t)core))   /* CPUTargetList: target self (same path send_cpu_interrupt uses) */
                  | (CPU_IRQ_PING & 0xFu);

        uint32_t guard = 0u;
        while (!irq_ping_done && guard < 200000u) { guard++; __asm__ __volatile__("" ::: "memory"); }

        g_irqlat[i] = irq_ping_done ? (irq_ping_t1 - t0) : 0xFFFFFFFFu;   /* sentinel: ping lost, don't hang */
    }

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    GICD_ISENABLER0 = (1u << 27);   /* restore the timer PPI we masked */
}


/* --- mutex ----------------------------------------------------------------- */

HOST_SHARED uint32_t g_mutex_unc[MUTEX_SAMPLES];
HOST_SHARED uint32_t g_mutex_con[MUTEX_SAMPLES];

static mutex_t micro_mutex;                 /* Normal cacheable -- ldrex/strex need it */
static volatile uint32_t mutex_hold_stop;


/* CPU_MUTEX_HOLD ISR body -- CPU1 holds/releases the lock in a tight loop until
 * CPU0 signals stop, creating real contention for CPU0's acquire measurement. */
void ktrace_mutex_hold(void)
{
    while (!mutex_hold_stop)
    {
        lock_mutex_persistent(&micro_mutex);
        for (volatile uint32_t d = 0; d < MUTEX_HOLD_WORK; d++) { }
        unlock_mutex(&micro_mutex);

        for (volatile uint32_t b = 0; b < MUTEX_BACKOFF; b++) { }   /* back off so CPU0 can win */
    }
}


static void mutex_run(void)
{
    micro_mutex    = 0u;
    mutex_hold_stop = 0u;

    /* Interrupts off throughout: the uncontended pair must not be preempted, and
     * the contended number must reflect only cross-core contention, not a tick. */
    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    for (uint32_t i = 0; i < MUTEX_SAMPLES; i++)
    {
        uint32_t t0 = pmu_cycles();
        lock_mutex_persistent(&micro_mutex);
        unlock_mutex(&micro_mutex);
        g_mutex_unc[i] = pmu_cycles() - t0;
    }

#if ENABLE_SMP
    send_cpu_interrupt(CPU_MUTEX_HOLD);     /* wake CPU1's holder loop */

    for (volatile uint32_t w = 0; w < MUTEX_SPINUP; w++) { }   /* let it get holding */

    for (uint32_t i = 0; i < MUTEX_SAMPLES; i++)
    {
        uint32_t t0 = pmu_cycles();

        /* Bounded retry rather than a blocking wfe: a tight-looping holder on CPU1
         * would starve a blocking waiter forever (that was the hang). */
        uint32_t guard = 0u;
        while (lock_mutex_best_effort(&micro_mutex) != LOCK_OK && guard < MUTEX_CON_CAP) guard++;

        uint32_t dt = pmu_cycles() - t0;

        if (guard < MUTEX_CON_CAP)
        {
            unlock_mutex(&micro_mutex);
            g_mutex_con[i] = dt;
        }
        else
        {
            g_mutex_con[i] = 0xFFFFFFFFu;   /* never won within cap -- don't hang */
        }
    }

    __asm__ __volatile__("dmb sy" ::: "memory");
    mutex_hold_stop = 1u;
    __asm__ __volatile__("dmb sy" ::: "memory");
#endif

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");
}


/* --- uncached memory-access latency (SDRAM vs OCRAM) ----------------------- */

HOST_SHARED uint32_t g_memlat_sdram_rd[MEMLAT_SAMPLES];
HOST_SHARED uint32_t g_memlat_sdram_wr[MEMLAT_SAMPLES];
HOST_SHARED uint32_t g_memlat_ocram_rd[MEMLAT_SAMPLES];
HOST_SHARED uint32_t g_memlat_ocram_wr[MEMLAT_SAMPLES];
HOST_SHARED uint32_t g_memlat_baseline;             /* empty-window pmu+dsb overhead */

HOST_SHARED       volatile uint32_t g_memlat_sdram_target;   /* uncached SDRAM word (.telemetry) */
HOST_SHARED_OCRAM volatile uint32_t g_memlat_ocram_target;   /* uncached OCRAM word (.host_ocram) */


static volatile uint32_t memlat_sink;   /* consume the loaded value so it isn't dead-code-eliminated */


/* Read and write latency of a single uncached access, measured independently.
 * Both targets are Strongly-ordered (Device-mapped), so nothing is cached. The
 * two directions are genuinely different paths, hence separate distributions:
 *  - write: posted -- the DDR controller/interconnect accepts+acks before the data
 *    lands, so it measures the accept path (constant, and SDRAM's posted path beats
 *    OCRAM's interconnect ack).
 *  - read: cannot be posted, the CPU stalls for the data, so it is the true access
 *    latency (OCRAM fast + flat; SDRAM slower with the refresh/row spread).
 * The dsb waits for the access to complete so it lands inside the timed window;
 * for reads the volatile load is consumed via memlat_sink. The baseline is the same
 * window with no access -- the common-mode pmu+dsb cost the host can subtract.
 * noinline keeps one copy of each loop (OCRAM .text is tight). */
__attribute__((noinline))
static void memlat_read_loop(const volatile uint32_t* target, uint32_t* out)
{
    for (uint32_t i = 0; i < MEMLAT_SAMPLES; i++)
    {
        uint32_t t0 = pmu_cycles();
        uint32_t v  = *target;
        __asm__ __volatile__("dsb sy" ::: "memory");
        out[i] = pmu_cycles() - t0;
        memlat_sink = v;
    }
}

__attribute__((noinline))
static void memlat_write_loop(volatile uint32_t* target, uint32_t* out)
{
    for (uint32_t i = 0; i < MEMLAT_SAMPLES; i++)
    {
        uint32_t t0 = pmu_cycles();
        *target = i;
        __asm__ __volatile__("dsb sy" ::: "memory");
        out[i] = pmu_cycles() - t0;
    }
}

static void memlat_run(void)
{
    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    uint32_t t0 = pmu_cycles();
    __asm__ __volatile__("dsb sy" ::: "memory");
    g_memlat_baseline = pmu_cycles() - t0;

    memlat_read_loop (&g_memlat_sdram_target, g_memlat_sdram_rd);
    memlat_write_loop(&g_memlat_sdram_target, g_memlat_sdram_wr);
    memlat_read_loop (&g_memlat_ocram_target, g_memlat_ocram_rd);
    memlat_write_loop(&g_memlat_ocram_target, g_memlat_ocram_wr);

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");
}


/* Progress marker the host polls to localize a stall: 1=ctxsw 2=irqlat 3=mutex
 * 4=memlat 5=done. If the suite ever hangs, the phase that failed to advance names it. */
HOST_SHARED volatile uint32_t g_micro_phase;


void micro_run(void)
{
    g_micro_phase = 1u;
    ctxsw_run();

    g_micro_phase = 2u;
    irqlat_run();

    g_micro_phase = 3u;
    mutex_run();

    g_micro_phase = 4u;
    memlat_run();

    g_micro_phase = 5u;
    KTRACE_MICRO_DONE();
}


#endif  /* MODE_TEST */
