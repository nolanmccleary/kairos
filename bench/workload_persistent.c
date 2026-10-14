#include <stdint.h>
#include "cpu.h"
#include "fault.h"
#include "ktrace.h"
#include "pmu.h"
#include "preempt_sched.h"
#include "telemetry.h"
#include "thread.h"
#include "tlsf.h"


/* -------------------------------------------------------------------------
 * Persistent-thread test.
 *
 * Builds a tight periodic taskset (hard EDF, auto-balanced across both cores)
 * and layers persistent (round-robin, best-effort) threads under it, then
 * traces the schedule so the host can see:
 *
 *   1. periodic always wins   -- periodic meet their deadlines (misses == 0)
 *      even at high utilization; persistent only ever run in the slack.
 *   2. persistent round-robin -- several threads at the SAME priority share the
 *      leftover evenly (equal run-ticks over a full rotation).
 *   3. strict priority levels -- MED/LOW persistent threads get ZERO time while
 *      a HIGH-priority persistent thread is alive.
 *   4. the balancer piles     -- add_persistent_thread balances on utilization,
 *      but an un-started persistent thread contributes 0 to it, so they all land
 *      on the same (less periodic-loaded) core. The other core's slack is wasted
 *      (idle) while the pile-on core's persistent threads are squeezed. The test
 *      records each thread's assigned core so the host can show this.
 *
 * Self-contained: brings the scheduler up, runs one measurement window, kills
 * its threads, tears back down. Reuses workload_edf.c's per-tick hook
 * (ktrace_edf_tick) via the shared trace_active flag + g_sched_trace buffer.
 * ------------------------------------------------------------------------- */

#define PP_NPERIODIC   6u
#define PP_NPERSIST    5u

/* Summed utilization of ALL periodic tasks. The balancer splits them ~evenly,
 * so each core lands near PP_U_PERMILLE / (1000 * NUM_CPUS). 1500 -> ~0.75/core:
 * tight enough to pressure EDF, loose enough that the ~0.25/core slack lets the
 * persistent threads complete a few round-robin rotations inside the trace
 * window. Raise it for a tighter squeeze (fewer persistent rounds visible). */
#define PP_U_PERMILLE  1500u

#define PP_CALIB_ITERS 200000u


/* Trace ids. Periodic 0..5; persistent in disjoint decades so the host buckets
 * them by range. HIGH {10,11,12} share a level (round-robin among them); MED and
 * LOW sit below and should never run while a HIGH thread is alive. */
#define PP_ID_HI_0   10u
#define PP_ID_HI_1   11u
#define PP_ID_HI_2   12u
#define PP_ID_MED    20u
#define PP_ID_LOW    30u


const uint32_t g_pp_periods[PP_NPERIODIC] = { 40u, 50u, 60u, 80u, 100u, 120u };

const uint32_t g_pp_persist_ids[PP_NPERSIST]  = { PP_ID_HI_0, PP_ID_HI_1, PP_ID_HI_2, PP_ID_MED, PP_ID_LOW };
const uint32_t g_pp_persist_prio[PP_NPERSIST] = { HIGH_PRIORITY, HIGH_PRIORITY, HIGH_PRIORITY, MED_PRIORITY, LOW_PRIORITY };

/* Host-visible outcomes (uncached host_shared, read straight over JTAG). */
HOST_SHARED volatile uint32_t g_pp_periodic_assign[PP_NPERIODIC];   /* periodic id -> core the balancer chose */
HOST_SHARED volatile uint32_t g_pp_periodic_done[PP_NPERIODIC];     /* periodic per-task completions */
HOST_SHARED volatile uint32_t g_pp_persist_assign[PP_NPERSIST];     /* persistent id -> core the balancer chose */

/* Owned by workload_edf.c; reused here. */
extern uint32_t trace_active;
extern volatile uint32_t g_trace_len[NUM_CPUS];


static uint32_t pp_iters[PP_NPERIODIC];


/* -------------------------------------------------------------------------
 * Jobs
 * ------------------------------------------------------------------------- */

static uint32_t pp_dummy = 0;
__attribute__((noinline, noclone))
static void pp_burn(uint32_t iters)
{
    for (uint32_t i = 0; i < iters; i++)
    {
        pp_dummy++;
    }
}


/* Periodic body: every balanced task runs this, reading its own id. Each lands
 * on exactly one core (balanced, added once), so g_pp_periodic_done is single-
 * writer per id -- no cross-core double count. */
static sys_exit_e pp_periodic_job(void)
{
    uint32_t id = g_cpus[curr_core()].curr_thread->id;

    pp_burn(pp_iters[id]);

    g_pp_periodic_done[id]++;
    return SYS_OK;
}


/* Persistent bodies: infinite loops (a persistent thread that returns just
 * FINISHES -- there is no re-release), so they run continuously and are sliced
 * by the scheduler. Four genuinely different inner loops: the scheduler treats
 * them identically, but distinct work (and distinct code) keeps the compiler
 * from folding them and makes each one's Gantt row mean something. volatile
 * sinks so none of it is dead-code-eliminated. */
static volatile uint32_t pp_sink_a, pp_sink_b, pp_sink_c, pp_sink_d, pp_sink_e;
static uint32_t pp_walk_buf[2048];

static sys_exit_e pp_hi_accum(void)      /* integer multiply/accumulate churn */
{
    uint32_t acc = 1u;
    for (;;)
    {
        acc = acc * 2654435761u + 1u;
        pp_sink_a = acc;
    }
}

static sys_exit_e pp_hi_walk(void)       /* strided memory walk (cache pressure) */
{
    uint32_t i = 0u;
    for (;;)
    {
        pp_walk_buf[i & 2047u] += i;
        i += 17u;
        pp_sink_b = pp_walk_buf[i & 2047u];
    }
}

static sys_exit_e pp_hi_lcg(void)        /* linear-congruential / running checksum */
{
    uint32_t x = 0x12345678u;
    for (;;)
    {
        x = x * 1664525u + 1013904223u;
        pp_sink_c ^= x;
    }
}

static sys_exit_e pp_med_popcount(void)  /* bit-population count sweep */
{
    uint32_t x = 1u;
    for (;;)
    {
        x += 1u;
        pp_sink_d += (uint32_t)__builtin_popcount(x);
    }
}

static sys_exit_e pp_low_spin(void)      /* bare spin */
{
    for (;;)
    {
        pp_sink_e++;
    }
}


static sys_exit_e (*const PP_PERSIST_JOBS[PP_NPERSIST])(void) =
{
    pp_hi_accum,
    pp_hi_walk,
    pp_hi_lcg,
    pp_med_popcount,
    pp_low_spin
};


/* -------------------------------------------------------------------------
 * Calibration (self-contained; mirrors workload_edf.c's, sized to pp_burn)
 * ------------------------------------------------------------------------- */

static inline uint32_t pp_rd_ticks(void)
{
    return *(volatile uint32_t*)&g_cpus[curr_core()].ticks;
}

static uint32_t pp_cycles_per_tick(void)
{
    uint32_t tick = pp_rd_ticks();
    while (pp_rd_ticks() == tick) {}

    uint32_t start = pmu_cycles();
    tick = pp_rd_ticks();

    while (pp_rd_ticks() < tick + 16u) {}

    return (pmu_cycles() - start) / 16u;
}

static uint32_t pp_cycles_per_iter(void)
{
    __asm__ __volatile__("cpsid i" ::: "memory");

    uint32_t start = pmu_cycles();
    pp_burn(PP_CALIB_ITERS);
    uint32_t cycles = pmu_cycles() - start;

    __asm__ __volatile__("cpsie i" ::: "memory");

    return cycles / PP_CALIB_ITERS;
}


/* -------------------------------------------------------------------------
 * Benchmark
 * ------------------------------------------------------------------------- */

void persistent_run(void)
{
    pmu_init();
    heap_init();            /* guarded: no-op if already live */
    psched_init();

    for (uint32_t c = 0; c < NUM_CPUS; c++)
    {
        g_trace_len[c] = 0u;
    }

    uint32_t cycles_per_tick = pp_cycles_per_tick();
    uint32_t cycles_per_iter = pp_cycles_per_iter();

    for (uint32_t i = 0; i < PP_NPERIODIC; i++)
    {
        uint32_t Ci_ticks = (PP_U_PERMILLE * g_pp_periods[i]) / (1000u * PP_NPERIODIC);
        if (Ci_ticks < 1u) Ci_ticks = 1u;

        pp_iters[i] = (uint32_t)((uint64_t)Ci_ticks * cycles_per_tick / cycles_per_iter);
        g_pp_periodic_done[i] = 0u;
    }

    g_test_release = 0u;

    thread_t* periodic_handles[PP_NPERIODIC];
    thread_t* persist_handles[PP_NPERSIST];

    /* Construction must not race the scheduler. */
    __asm__ __volatile__("cpsid i" ::: "memory");

    trace_active = 1u;

    for (uint32_t i = 0; i < PP_NPERIODIC; i++)
    {
        periodic_handles[i]     = add_periodic_thread(pp_periodic_job, g_pp_periods[i], PERIODIC, i);
        g_pp_periodic_assign[i] = (uint32_t)periodic_handles[i]->core;
    }

    for (uint32_t i = 0; i < PP_NPERSIST; i++)
    {
        persist_handles[i]     = add_persistent_thread(PP_PERSIST_JOBS[i], g_pp_persist_ids[i],
                                                       (persistent_thread_priority_e)g_pp_persist_prio[i]);
        g_pp_persist_assign[i] = (uint32_t)persist_handles[i]->core;
    }

    __asm__ __volatile__("dmb sy" ::: "memory");

    /* Assignment is final here (nothing has run) -- host reads g_pp_*_assign. */
    KTRACE_PERSIST_READY();

    __asm__ __volatile__("cpsie i" ::: "memory");

    /* Host samples the schedule trace after a measurement window, then releases. */
    KTRACE_WAIT_RELEASE();

    __asm__ __volatile__("cpsid i" ::: "memory");

    for (uint32_t i = 0; i < PP_NPERIODIC; i++)
    {
        kill_thread(periodic_handles[i]);
    }
    for (uint32_t i = 0; i < PP_NPERSIST; i++)
    {
        kill_thread(persist_handles[i]);
    }

    __asm__ __volatile__("cpsie i" ::: "memory");

    psched_deinit();

    KTRACE_PERSIST_DONE();
}
