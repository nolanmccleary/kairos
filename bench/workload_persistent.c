#include <stdint.h>
#include "cpu.h"
#include "fault.h"
#include "ktrace.h"
#include "pmu.h"
#include "sched.h"
#include "telemetry.h"
#include "thread.h"
#include "tlsf.h"


/* -------------------------------------------------------------------------
 * Persistent-thread test.
 *
 * Builds a tight periodic taskset (hard EDF, auto-balanced across both cores) and layers
 * persistent (round-robin, best-effort) threads under it, then traces the schedule so the
 * host can see:
 *
 *   1. periodic always wins   -- periodic meet their deadlines (misses == 0) even at high
 *      utilization; persistent only ever run in the slack.
 *   2. persistent round-robin -- several threads at the SAME priority share the leftover
 *      evenly (equal run-ticks over a full rotation).
 *   3. strict priority levels -- medium/low persistent threads get ZERO time while a high
 *      persistent thread is alive.
 *   4. the balancer piles     -- add_persistent_thread balances on utilization, but an
 *      un-started persistent thread contributes 0 to it, so they all land on the same
 *      (less periodic-loaded) core. The other core's slack is wasted (idle) while the
 *      pile-on core's persistent threads are squeezed. The test records each thread's
 *      assigned core so the host can show this.
 *
 * Self-contained: brings the scheduler up, runs one measurement window, kills its threads,
 * tears back down. Reuses workload_edf.c's per-tick hook (ktrace_edf_tick) via the shared
 * trace_active flag + g_sched_trace buffer.
 * ------------------------------------------------------------------------- */

#define PERIODIC_TASK_COUNT      6u
#define PERSISTENT_THREAD_COUNT  5u

/* Summed utilization of ALL periodic tasks, scaled by core count so the balancer lands
 * ~0.75/core whether dual (1500) or single (750). Tight enough to pressure EDF, loose
 * enough that the ~0.25/core slack lets the persistent threads complete a few round-robin
 * rotations inside the trace window. */
#define PERIODIC_UTILIZATION_PERMILLE  (750u * NUM_CPUS)

#define CALIBRATION_ITERS  200000u

#define WALK_BUFFER_LEN    2048u   /* high_priority_memory_walk stride buffer; power of two for the mask */


/* Trace ids. Periodic 0..5; persistent in disjoint decades so the host buckets them by
 * range. The three high-priority ids share a level (round-robin among them); medium and
 * low sit below and should never run while a high-priority thread is alive. */
#define HIGH_PRIORITY_ID_0   10u
#define HIGH_PRIORITY_ID_1   11u
#define HIGH_PRIORITY_ID_2   12u
#define MEDIUM_PRIORITY_ID   20u
#define LOW_PRIORITY_ID      30u


/* Taskset descriptors are compile-time constants the host reads over JTAG. Kept as loaded
 * consts in OCRAM (like workload_edf.c's) so they carry their values from boot, readable
 * even at the pre-SDRAM boot gate, with no runtime populate step. */
const uint32_t g_persistent_periodic_periods[PERIODIC_TASK_COUNT] = { 40u, 50u, 60u, 80u, 100u, 120u };

const uint32_t g_persistent_thread_ids[PERSISTENT_THREAD_COUNT] =
    { HIGH_PRIORITY_ID_0, HIGH_PRIORITY_ID_1, HIGH_PRIORITY_ID_2, MEDIUM_PRIORITY_ID, LOW_PRIORITY_ID };
const uint32_t g_persistent_thread_priorities[PERSISTENT_THREAD_COUNT] =
    { HIGH_PRIORITY, HIGH_PRIORITY, HIGH_PRIORITY, MED_PRIORITY, LOW_PRIORITY };

/* Host-visible outcomes, read straight over JTAG. */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_persistent_periodic_assign[PERIODIC_TASK_COUNT];  /* periodic id -> core the balancer chose   */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_persistent_periodic_done[PERIODIC_TASK_COUNT];    /* periodic per-task completions            */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_persistent_thread_assign[PERSISTENT_THREAD_COUNT];/* persistent id -> core the balancer chose */

/* trace_active + g_trace_len are shared trace state declared in ktrace.h. */


static uint32_t periodic_task_iters[PERIODIC_TASK_COUNT];


/* -------------------------------------------------------------------------
 * Jobs
 * ------------------------------------------------------------------------- */

static uint32_t work_sink = 0;
__attribute__((noinline, noclone))
static void burn_iterations(uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; i++)
    {
        work_sink++;
    }
}


/* Periodic body: every balanced task runs this, reading its own id. Each lands on exactly
 * one core (balanced, added once), so g_persistent_periodic_done is single-writer per id. */
static sys_exit_e periodic_task_job(void)
{
    uint32_t task_id = g_cpus[curr_core()].curr_thread->id;

    burn_iterations(periodic_task_iters[task_id]);

    g_persistent_periodic_done[task_id]++;
    return SYS_OK;
}


/* Persistent bodies: infinite loops (a persistent thread that returns just FINISHES --
 * there is no re-release), so they run continuously and are sliced by the scheduler. Four
 * genuinely different inner loops so each one's Gantt row means something and the compiler
 * can't fold them; volatile sinks keep them from being dead-code-eliminated. */
static volatile uint32_t accumulate_sink, walk_sink, checksum_sink, bit_count_sink, spin_sink;
static uint32_t walk_buffer[WALK_BUFFER_LEN];

static sys_exit_e high_priority_accumulate(void)    /* integer multiply/accumulate churn */
{
    uint32_t accumulator = 1u;
    for (;;)
    {
        accumulator = accumulator * 2654435761u + 1u;
        accumulate_sink = accumulator;
    }
}

static sys_exit_e high_priority_memory_walk(void)   /* strided memory walk (cache pressure) */
{
    uint32_t walk_index = 0u;
    for (;;)
    {
        walk_buffer[walk_index & (WALK_BUFFER_LEN - 1u)] += walk_index;
        walk_index += 17u;
        walk_sink = walk_buffer[walk_index & (WALK_BUFFER_LEN - 1u)];
    }
}

static sys_exit_e high_priority_checksum(void)      /* running checksum via linear-congruential step */
{
    uint32_t checksum_state = 0x12345678u;
    for (;;)
    {
        checksum_state = checksum_state * 1664525u + 1013904223u;
        checksum_sink ^= checksum_state;
    }
}

static sys_exit_e medium_priority_bit_count(void)   /* bit-population count sweep */
{
    uint32_t value = 1u;
    for (;;)
    {
        value += 1u;
        bit_count_sink += (uint32_t)__builtin_popcount(value);
    }
}

static sys_exit_e low_priority_spin(void)           /* bare spin */
{
    for (;;)
    {
        spin_sink++;
    }
}


static sys_exit_e (*const PERSISTENT_JOBS[PERSISTENT_THREAD_COUNT])(void) =
{
    high_priority_accumulate,
    high_priority_memory_walk,
    high_priority_checksum,
    medium_priority_bit_count,
    low_priority_spin
};


/* -------------------------------------------------------------------------
 * Calibration (self-contained; mirrors workload_edf.c's, sized to burn_iterations)
 * ------------------------------------------------------------------------- */

static inline uint32_t read_ticks(void)
{
    return *(volatile uint32_t*)&g_cpus[curr_core()].ticks;
}

static uint32_t measure_cycles_per_tick(void)
{
    uint32_t tick = read_ticks();
    while (read_ticks() == tick) {}

    uint32_t start = pmu_cycles();
    tick = read_ticks();

    while (read_ticks() < tick + 16u) {}

    return (pmu_cycles() - start) / 16u;
}

static uint32_t measure_cycles_per_iter(void)
{
    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    uint32_t start = pmu_cycles();
    burn_iterations(CALIBRATION_ITERS);
    uint32_t cycles = pmu_cycles() - start;

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    return cycles / CALIBRATION_ITERS;
}


/* -------------------------------------------------------------------------
 * Benchmark
 * ------------------------------------------------------------------------- */

void persistent_run(void)
{
    pmu_init();
    heap_init();            /* guarded: no-op if already live */
    psched_init();

    for (uint32_t core = 0; core < NUM_CPUS; core++)
    {
        g_trace_len[core] = 0u;
    }

    uint32_t cycles_per_tick = measure_cycles_per_tick();
    uint32_t cycles_per_iter = measure_cycles_per_iter();

    for (uint32_t i = 0; i < PERIODIC_TASK_COUNT; i++)
    {
        uint32_t compute_ticks = (PERIODIC_UTILIZATION_PERMILLE * g_persistent_periodic_periods[i]) / (1000u * PERIODIC_TASK_COUNT);
        if (compute_ticks < 1u) compute_ticks = 1u;

        periodic_task_iters[i]         = (uint32_t)((uint64_t)compute_ticks * cycles_per_tick / cycles_per_iter);
        g_persistent_periodic_done[i]  = 0u;
    }

    g_test_release = 0u;

    thread_t* periodic_handles[PERIODIC_TASK_COUNT];
    thread_t* persistent_handles[PERSISTENT_THREAD_COUNT];

    /* Construction must not race the scheduler. */
    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    trace_active = 1u;

    for (uint32_t i = 0; i < PERIODIC_TASK_COUNT; i++)
    {
        periodic_handles[i]            = add_periodic_thread(kMalloc(sizeof(thread_t)), periodic_task_job, g_persistent_periodic_periods[i], PERIODIC, i);
        g_persistent_periodic_assign[i] = (uint32_t)periodic_handles[i]->core;
    }

    for (uint32_t i = 0; i < PERSISTENT_THREAD_COUNT; i++)
    {
        persistent_handles[i]         = add_persistent_thread(kMalloc(sizeof(thread_t)), PERSISTENT_JOBS[i], g_persistent_thread_ids[i],
                                                              (persistent_thread_priority_e)g_persistent_thread_priorities[i]);
        g_persistent_thread_assign[i] = (uint32_t)persistent_handles[i]->core;
    }

    __asm__ __volatile__("dmb sy" ::: "memory");

    /* Assignment is final here (nothing has run) -- host reads g_persistent_*_assign. */
    KTRACE_PERSIST_READY();

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    /* Host samples the schedule trace after a measurement window, then releases. */
    KTRACE_WAIT_RELEASE();

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    for (uint32_t i = 0; i < PERIODIC_TASK_COUNT; i++)
    {
        kill_thread(periodic_handles[i]);
    }
    for (uint32_t i = 0; i < PERSISTENT_THREAD_COUNT; i++)
    {
        kill_thread(persistent_handles[i]);
    }

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    psched_deinit();

    for (uint32_t i = 0; i < PERIODIC_TASK_COUNT; i++)     if (periodic_handles[i])   kFree(periodic_handles[i]);
    for (uint32_t i = 0; i < PERSISTENT_THREAD_COUNT; i++) if (persistent_handles[i]) kFree(persistent_handles[i]);

    KTRACE_PERSIST_DONE();
}
