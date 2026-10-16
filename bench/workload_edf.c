#include <stdint.h>
#include "cpu.h"
#include "fault.h"
#include "ktrace.h"
#include "pmu.h"
#include "sched.h"
#include "telemetry.h"
#include "thread.h"
#include "tlsf.h"

#define NTASKS       3u
#define CALIB_ITERS  200000u

#define TRACE_TICKS  7200u          /* per-tick schedule-trace window for the host Gantt; 12 hyperperiods (lcm(40,60,100)=600), also sized so the persistent test captures several round-robin rounds under tight periodic load */

#define ARRAY_LEN(a) \
    (sizeof(a) / sizeof((a)[0]))


/* -------------------------------------------------------------------------
 * Host-visible experiment state
 * ------------------------------------------------------------------------- */

const uint32_t g_edf_u_values[] =
{
    500,
    700,
    850,
    900,
    950,
    975,
    1000,
    1025,
    1050,
    1100
};


/* volatile so the firmware actually loads it (no const-fold) and the symbol
 * survives --gc-sections -- the host reads it to learn the sweep length. */
const volatile uint32_t g_edf_u_count = ARRAY_LEN(g_edf_u_values);


const uint32_t g_edf_periods[NTASKS] =
{
    40,
    60,
    100
};


/* EDF test/JTAG state -- host_shared (uncached), read/written directly. These
 * are pure test payload, not perf-critical system data, so no caching wanted. */
HOST_SHARED volatile uint32_t g_edf_u_index;
HOST_SHARED volatile uint32_t g_edf_u_permille;

HOST_SHARED volatile uint32_t g_edf_C[NTASKS];
HOST_SHARED volatile uint32_t g_edf_done[NTASKS];

HOST_SHARED volatile uint8_t  g_sched_trace[NUM_CPUS][TRACE_TICKS];
HOST_SHARED volatile uint32_t g_trace_len[NUM_CPUS];

uint32_t trace_active;   /* 1 while a trial is running (every trial is traced); shared with workload_persistent.c */
static uint32_t iters[NTASKS];


/* Per-task metrics mirror. The per-tick hook copies the running task's cached
 * metrics_t here; the region is uncached (.telemetry), so a JTAG phys read at the
 * trial halt sees fresh ci_av/ti_av with no cache maintenance -- the CPU copy reads
 * its own fresh cache and writes memory the host reads directly. Index by task. */
/* MIRRORS: cached, system-owned globals cloned into host_shared every tick by
 * ktrace_edf_tick, so the debugger reads coherent values without the CPU paying
 * for uncached access on the hot paths. */
HOST_SHARED metrics_t g_edf_metrics[NTASKS];   /* <- running->metrics (cached thread_t)  */
HOST_SHARED uint32_t  g_ticks_m[NUM_CPUS];
HOST_SHARED uint32_t  g_misses_m[NUM_CPUS];
HOST_SHARED uint32_t  g_overhead_m[NUM_CPUS];
HOST_SHARED uint32_t  g_core_u_m[NUM_CPUS];   /* per-core utilization, fixed-point /65536 */



/* -------------------------------------------------------------------------
 * Thread-corruption watchdog (test harness).
 *
 * The harness created every task, so it knows each field's ground truth. The
 * per-tick hook re-validates the live tasks against that truth; the first
 * mismatch captures a full report here (host-readable) and traps, so we see
 * WHICH task, WHICH field, and expected-vs-actual the instant the struct rots
 * -- before next_up's generic status/periodicity trap fires downstream.
 * ------------------------------------------------------------------------- */
static thread_t*         g_edf_handles[NUM_CPUS][NTASKS];  /* this trial's tasks, per core */
static volatile uint32_t g_edf_armed;                      /* 1 while a trial's tasks are live */

/* Premature-FINISH detector. A task's job is the ONLY legitimate way it finishes,
 * so the job bumps g_task_completions right before returning. On entry it checks
 * that the previous instance actually completed (completions advanced since the
 * snapshot it took last entry); if not, the scheduler re-ran the task without it
 * finishing -> a premature FINISH. Per (core, task); g_task_started gates the very
 * first instance and is cleared per trial. */
static volatile uint32_t g_task_completions[NUM_CPUS][NTASKS];
static volatile uint32_t g_task_started[NUM_CPUS][NTASKS];
static volatile uint32_t g_task_start_snap[NUM_CPUS][NTASKS];
static volatile uint32_t g_task_trial[NUM_CPUS][NTASKS];   /* g_edf_u_index the tracking is for */

HOST_SHARED volatile uint32_t g_edf_bad_thread;    /* offending thread_t*                  */
HOST_SHARED volatile uint32_t g_edf_bad_core;      /* core whose watchdog tripped          */
HOST_SHARED volatile uint32_t g_edf_bad_task;      /* task index 0..NTASKS-1               */
HOST_SHARED volatile uint32_t g_edf_bad_expected;  /* value the harness set                */
HOST_SHARED volatile uint32_t g_edf_bad_actual;    /* value found                          */
HOST_SHARED volatile uint32_t g_edf_bad_tick;      /* tick at detection                    */



/* -------------------------------------------------------------------------
 * Synthetic jobs
 * ------------------------------------------------------------------------- */

static uint32_t dummy = 0;
__attribute__((noinline, noclone))
static void do_work(uint32_t iters)
{
    for (uint32_t i = 0; i < iters; i++)
    {
        dummy++;
    }
}


/* Both cores run these same jobs concurrently, but only CPU0's completions are
 * tracked, so guard g_edf_done -- otherwise CPU1's jobs double-count it. */
/* Shared task body. Self-polices premature FINISH: on entry, the previous
 * instance must have bumped g_task_completions since this task last snapshotted
 * it; if it didn't, the scheduler re-ran the task without it finishing. */
static void job_body(uint32_t idx)
{
    cpu_core_e core = curr_core();

    /* New trial? drop this task's tracking so its first instance doesn't get
     * measured against the prior trial (whose last instance kill_thread cut short). */
    if (g_task_trial[core][idx] != g_edf_u_index)
    {
        g_task_trial[core][idx]   = g_edf_u_index;
        g_task_started[core][idx] = 0u;
    }

    if (g_task_started[core][idx] &&
        g_task_completions[core][idx] == g_task_start_snap[core][idx])
    {
        raise_error_ctx("edf watchdog: task re-run without completing prior instance (premature FINISH)",
                        (uint32_t)(uintptr_t)g_edf_handles[core][idx]);
    }

    g_task_started[core][idx]    = 1u;
    g_task_start_snap[core][idx] = g_task_completions[core][idx];

    do_work(iters[idx]);

    if (core == CPU0) g_edf_done[idx]++;

    g_task_completions[core][idx]++;   /* the one legitimate finish signal */
}


static sys_exit_e job0(void) { job_body(0u); return SYS_OK; }
static sys_exit_e job1(void) { job_body(1u); return SYS_OK; }
static sys_exit_e job2(void) { job_body(2u); return SYS_OK; }


static sys_exit_e (*const JOBS[NTASKS])(void) =
{
    job0,
    job1,
    job2
};


/* Record the corruption for the host, then trap. raise_error() does not return. */
static void edf_flag_corruption(const char* msg, thread_t* t, cpu_core_e core,
                                uint32_t idx, uint32_t expected, uint32_t actual)
{
    g_edf_bad_thread   = (uint32_t)(uintptr_t)t;
    g_edf_bad_core     = (uint32_t)core;
    g_edf_bad_task     = idx;
    g_edf_bad_expected = expected;
    g_edf_bad_actual   = actual;
    g_edf_bad_tick     = g_cpus[core].ticks;

    __asm__ volatile("dmb sy" ::: "memory");
    raise_error_ctx(msg, (uint32_t)(uintptr_t)t);
}


/* Validate one live task against the values add_periodic_thread_to_core() stamped. Fields
 * period/func/periodicity/core are set once and never change; thread_status must
 * stay a valid enum; sp must live inside the task's own stack. Any deviation ==
 * the struct got clobbered (freed + reused, stray write, etc.). */
static void edf_check_task(cpu_core_e core, uint32_t idx)
{
    thread_t* t = g_edf_handles[core][idx];
    if (t == NULL) return;

    if (t->md.periodicity != PERIODIC)
        edf_flag_corruption("edf watchdog: task periodicity corrupted",
                            t, core, idx, (uint32_t)PERIODIC, (uint32_t)t->md.periodicity);

    if (t->timing.period != g_edf_periods[idx])
        edf_flag_corruption("edf watchdog: task period corrupted",
                            t, core, idx, g_edf_periods[idx], t->timing.period);

    if (t->func != JOBS[idx])
        edf_flag_corruption("edf watchdog: task func corrupted",
                            t, core, idx, (uint32_t)(uintptr_t)JOBS[idx], (uint32_t)(uintptr_t)t->func);

    if (t->core != core)
        edf_flag_corruption("edf watchdog: task core corrupted",
                            t, core, idx, (uint32_t)core, (uint32_t)t->core);

    if (t->md.status != PENDING && t->md.status != RUNNING && t->md.status != FINISHED)
        edf_flag_corruption("edf watchdog: task status corrupted",
                            t, core, idx, 0u, (uint32_t)t->md.status);

    if ((char*)t->sp < t->stack || (char*)t->sp > t->stack + THREAD_STACK_SIZE)
        edf_flag_corruption("edf watchdog: task sp outside its stack",
                            t, core, idx, (uint32_t)(uintptr_t)t->stack, (uint32_t)(uintptr_t)t->sp);
}


void ktrace_edf_tick(thread_t* running)
{
    if (!trace_active) return;

    cpu_core_e core = curr_core();
    uint32_t id = running->id;

    if (g_trace_len[core] < TRACE_TICKS)
    {
        g_sched_trace[core][g_trace_len[core]++] = (uint8_t)id;
    }

    if (core == CPU0 && id < NTASKS)
    {
        g_edf_metrics[id] = running->metrics;
    }

    g_ticks_m[core]    = g_cpus[core].ticks;
    g_misses_m[core]   = g_cpus[core].missed_deadlines;
    g_overhead_m[core] = g_cpus[core].avg_scheduler_overhead;
    g_core_u_m[core]   = g_cpus[core].utilization;

    /* Watchdog: re-check every live task on this core against ground truth. */
    if (g_edf_armed)
    {
        for (uint32_t i = 0; i < NTASKS; i++)
        {
            edf_check_task(core, i);
        }
    }
}


/* -------------------------------------------------------------------------
 * Calibration
 * ------------------------------------------------------------------------- */

//TODO: Implement mechanism to read kernel objects safely
// returns current gTicks value
static inline uint32_t rd_ticks(void)
{
    return *(volatile uint32_t*)&g_cpus[curr_core()].ticks;
}


//Calculates avg number of cycles per tick
static uint32_t measure_cycles_per_tick(void)
{
    uint32_t tick = rd_ticks();

    /*
     * Synchronize to a tick edge.
     */
    while (rd_ticks() == tick){}

    uint32_t start = pmu_cycles();
    tick = rd_ticks();


    while (rd_ticks() < tick + 16u){} //16 tick averaging window

    return (pmu_cycles() - start) / 16u;
}


//Runs the calibration burst and returns cycles per do_work iteration (>= 1)
static uint32_t measure_cycles_per_iter(void)
{
    __asm__ __volatile__(
        "cpsid i"
        :
        :
        : "memory"
    );


    uint32_t start = pmu_cycles();

    do_work(CALIB_ITERS);

    uint32_t cycles = pmu_cycles() - start;


    __asm__ __volatile__(
        "cpsie i"
        :
        :
        : "memory"
    );


    return cycles / CALIB_ITERS;   // cycles-per-iter: reciprocal is < 1 and would truncate to 0
}


/* -------------------------------------------------------------------------
 * Trial setup
 * ------------------------------------------------------------------------- */

static void configure_work(uint32_t u_permille, uint32_t cycles_per_tick, uint32_t cycles_per_iter)
{
    for (uint32_t i = 0; i < NTASKS; i++)
    {
        uint32_t Ci_ticks =(u_permille * g_edf_periods[i]) / (1000u * NTASKS);

        if (Ci_ticks < 1u) Ci_ticks = 1u;

        g_edf_C[i] = Ci_ticks;
        iters[i] = (uint32_t)((uint64_t)Ci_ticks * cycles_per_tick / cycles_per_iter);
    }
}


static void reset_trial(void)
{
    g_cpus[curr_core()].ticks = 0u;
    g_cpus[curr_core()].missed_deadlines = 0u;


    trace_active   = 1u;   /* trace every trial; the host reads each one and picks which to plot */

    for (uint32_t c = 0; c < NUM_CPUS; c++)
    {
        g_trace_len[c]         = 0u;
        g_cpus[c].avg_scheduler_overhead = 0u;   /* fresh per-CPU scheduler-overhead EWMA per trial */
    }


    for (uint32_t i = 0; i < NTASKS; i++)
    {
        g_edf_done[i]    = 0u;
        g_edf_metrics[i] = (metrics_t){0};
    }
}


/* -------------------------------------------------------------------------
 * Benchmark
 * ------------------------------------------------------------------------- */

void edf_run(void)
{
    pmu_init();

    /*
     * allocbench destroyed its heap before returning.
     */
    heap_init();
    psched_init();
    __asm__ __volatile__(
            "cpsie i"
            :
            :
            : "memory"
    );

    for (uint32_t c = 0; c < NUM_CPUS; c++)
    {
        g_ticks_m[c]    = 0u;
        g_misses_m[c]   = 0u;
        g_overhead_m[c] = 0u;
        g_core_u_m[c]   = 0u;
    }

    for (uint32_t i = 0; i < NTASKS; i++)
    {
        g_edf_metrics[i] = (metrics_t){0};
    }


    uint32_t cycles_per_tick = measure_cycles_per_tick();
    uint32_t cycles_per_iter = measure_cycles_per_iter();


    for (uint32_t trial = 0; trial < g_edf_u_count; trial++)
    {
        /*
         * Trial construction must not race the scheduler.
         */
        __asm__ __volatile__(
            "cpsid i"
            :
            :
            : "memory"
        );

        g_edf_u_index = trial;
        g_edf_u_permille = g_edf_u_values[trial];


        reset_trial();


        configure_work(
            g_edf_u_permille,
            cycles_per_tick,
            cycles_per_iter
        );


        g_test_release = 0u;
        g_edf_armed    = 0u;   /* don't validate a half-built task set */

        /* Both cores run the same workload concurrently. The allocator mutex makes
         * the shared heap safe, and each core's thread_mutex makes the cross-core
         * push into CPU1's incoming FIFO safe against CPU1's own scheduler. */
        for (uint32_t i = 0; i < NTASKS; i++)
        {
            g_edf_handles[CPU0][i] = add_periodic_thread_to_core(CPU0, JOBS[i], g_edf_periods[i], PERIODIC, i);
            g_edf_handles[CPU1][i] = add_periodic_thread_to_core(CPU1, JOBS[i], g_edf_periods[i], PERIODIC, i);
        }

        g_edf_armed = 1u;   /* task set complete -- watchdog live from here */

        __asm__ __volatile__(
            "dmb sy"
            :
            :
            : "memory"
        );


        /*
         * Python has a hardware breakpoint installed here.
         *
         * Reaching it means this trial is fully configured.
         */
        KTRACE_EDF_READY();

        __asm__ __volatile__(
            "cpsie i"
            :
            :
            : "memory"
        );


        /*
         * Main stays here while scheduler IRQs execute the EDF workload.
         *
         * Python:
         *
         *     resumes after the breakpoint
         *     waits 12 seconds
         *     halts target
         *     samples state
         *     writes g_test_release = 1
         *     resumes
         */
        KTRACE_WAIT_RELEASE();


        /*
         * Remove this trial before constructing the next one.
         */
        __asm__ __volatile__(
            "cpsid i"
            :
            :
            : "memory"
        );


        g_edf_armed = 0u;   /* tearing the trial down -- stop validating (kill flips fields) */

        // psched_clear_threads();
        for (uint32_t i = 0; i < NTASKS; i++)
        {
            kill_thread(g_edf_handles[CPU0][i]);
            kill_thread(g_edf_handles[CPU1][i]);
        }


        __asm__ __volatile__(
            "cpsie i"
            :
            :
            : "memory"
        );
    }


    /* Sweep complete on both cores -- tear the scheduler back down (CPU0 locally +
     * IPI to CPU1), mirroring the psched_init() at the top. */
    psched_deinit();

    KTRACE_EDF_DONE();
}



/* -------------------------------------------------------------------------
 * Load-balance test: six equal u=0.25 periodic tasks (total 1.5) handed to the
 * balancer (add_periodic_thread), which should spread them to ~0.75 on each core. Self-
 * contained: brings the scheduler up, runs, kills its tasks, tears back down.
 * Tasks are labelled by id; add_periodic_thread's return tells us which core each landed on.
 * ------------------------------------------------------------------------- */

#define BAL_NTASKS   6u

const uint32_t g_bal_periods[BAL_NTASKS] = { 40u, 40u, 60u, 60u, 100u, 100u };

HOST_SHARED volatile uint32_t g_bal_assign[BAL_NTASKS];   /* id -> assigned core */
HOST_SHARED volatile uint32_t g_bal_done[BAL_NTASKS];     /* per-task completions */

static uint32_t bal_iters[BAL_NTASKS];


/* One body for every balance task; each reads its own id from curr_thread. */
static sys_exit_e bal_job(void)
{
    uint32_t id = g_cpus[curr_core()].curr_thread->id;

    do_work(bal_iters[id]);

    g_bal_done[id]++;
    return SYS_OK;
}


void edf_balance_run(void)
{
    pmu_init();
    heap_init();            /* guarded: no-op if already live */
    psched_init();

    for (uint32_t c = 0; c < NUM_CPUS; c++)
    {
        g_trace_len[c] = 0u;
        g_core_u_m[c]  = 0u;
    }

    uint32_t cycles_per_tick = measure_cycles_per_tick();
    uint32_t cycles_per_iter = measure_cycles_per_iter();

    for (uint32_t i = 0; i < BAL_NTASKS; i++)
    {
        uint32_t Ci_ticks = g_bal_periods[i] / 4u;   /* u = 0.25 -> Ci = period/4 */
        if (Ci_ticks < 1u) Ci_ticks = 1u;

        bal_iters[i]  = (uint32_t)((uint64_t)Ci_ticks * cycles_per_tick / cycles_per_iter);
        g_bal_done[i] = 0u;
    }

    g_test_release = 0u;

    thread_t* handles[BAL_NTASKS];

    __asm__ __volatile__("cpsid i" ::: "memory");

    trace_active = 1u;

    for (uint32_t i = 0; i < BAL_NTASKS; i++)
    {
        handles[i] = add_periodic_thread(bal_job, g_bal_periods[i], PERIODIC, i);
        g_bal_assign[i] = (uint32_t)handles[i]->core;
    }

    __asm__ __volatile__("dmb sy" ::: "memory");

    KTRACE_BALANCE_READY();

    __asm__ __volatile__("cpsie i" ::: "memory");

    KTRACE_WAIT_RELEASE();

    __asm__ __volatile__("cpsid i" ::: "memory");

    for (uint32_t i = 0; i < BAL_NTASKS; i++)
    {
        kill_thread(handles[i]);
    }

    __asm__ __volatile__("cpsie i" ::: "memory");

    psched_deinit();

    KTRACE_BALANCE_DONE();
}
