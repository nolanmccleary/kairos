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
#define EDF_U_COUNT  10u            /* number of utilization points in the sweep */
#define CALIB_ITERS  200000u

/* Per-tick schedule-trace window for the host Gantt: 12 hyperperiods of lcm(40,60,100)=600
 * ticks, also long enough for the persistent test to capture several round-robin rounds. */
#define TRACE_TICKS  7200u


/* -------------------------------------------------------------------------
 * Host-visible experiment state
 * ------------------------------------------------------------------------- */

/* The sweep descriptors are compile-time constants the host reads over JTAG at the boot
 * gate, before cpu0_startup brings SDRAM up. So they must be loaded consts in OCRAM (the
 * boot ROM loads OCRAM before the gate); SDRAM telemetry would be both unpowered and
 * unpopulated at that read. volatile on the count so the firmware loads it (no const-fold)
 * and the symbol survives --gc-sections. */
const uint32_t g_edf_u_values[EDF_U_COUNT] =
{
    500u, 700u, 850u, 900u, 950u, 975u, 1000u, 1025u, 1050u, 1100u
};

const volatile uint32_t g_edf_u_count = EDF_U_COUNT;

const uint32_t g_edf_periods[NTASKS] =
{
    40u, 60u, 100u
};

SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_u_index;
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_u_permille;

SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_C[NTASKS];
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_done[NTASKS];

SDRAM_UNCACHED_TELEMETRY volatile uint8_t g_sched_trace[NUM_CPUS][TRACE_TICKS];
/* trace_active + g_trace_len are shared trace state, now defined in ktrace.c (ktrace.h). */

static uint32_t task_iters[NTASKS];


/* The per-tick hook copies each running task's cached metrics into these uncached mirrors
 * so a JTAG read at the trial halt sees fresh values without any cache maintenance. */
SDRAM_UNCACHED_TELEMETRY thread_metrics_t g_edf_metrics[NTASKS];
SDRAM_UNCACHED_TELEMETRY uint32_t         g_ticks_m[NUM_CPUS];
SDRAM_UNCACHED_TELEMETRY uint32_t         g_misses_m[NUM_CPUS];
SDRAM_UNCACHED_TELEMETRY uint32_t         g_overhead_m[NUM_CPUS];
SDRAM_UNCACHED_TELEMETRY uint32_t         g_core_u_m[NUM_CPUS];   /* per-core utilization, fixed-point /65536 */



/* -------------------------------------------------------------------------
 * Thread-corruption watchdog (test harness)
 *
 * The harness created every task, so it knows each field's ground truth. The per-tick
 * hook re-validates the live tasks against that truth; the first mismatch records a
 * report here and traps, so we see which task, which field, and expected-vs-actual the
 * instant the struct is clobbered.
 * ------------------------------------------------------------------------- */
static thread_t*         g_edf_handles[NUM_CPUS][NTASKS];   /* this trial's tasks, per core */
static volatile uint32_t g_edf_armed;                       /* 1 while a trial's tasks are live */

/* Premature-FINISH detector. A task's job is the only legitimate way it finishes, so the
 * job bumps g_task_completions right before returning. On entry it checks the previous
 * instance actually completed (completions advanced since its last snapshot); if not, the
 * scheduler re-ran the task without it finishing. Per (core, task). */
static volatile uint32_t g_task_completions[NUM_CPUS][NTASKS];
static volatile uint32_t g_task_started[NUM_CPUS][NTASKS];
static volatile uint32_t g_task_start_snap[NUM_CPUS][NTASKS];
static volatile uint32_t g_task_trial[NUM_CPUS][NTASKS];    /* g_edf_u_index the tracking is for */

SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_bad_thread;    /* offending thread_t*         */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_bad_core;      /* core whose watchdog tripped */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_bad_task;      /* task index 0..NTASKS-1      */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_bad_expected;  /* value the harness set       */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_bad_actual;    /* value found                 */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_edf_bad_tick;      /* tick at detection           */



/* -------------------------------------------------------------------------
 * Synthetic jobs
 * ------------------------------------------------------------------------- */

static uint32_t work_sink = 0;
__attribute__((noinline, noclone))
static void do_work(uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; i++)
    {
        work_sink++;
    }
}


/* Shared task body. Both cores run these same jobs concurrently, but only CPU0's
 * completions are tracked, so g_edf_done is guarded to CPU0 (otherwise CPU1's jobs
 * double-count it). Also self-polices premature FINISH via the detector above. */
static void job_body(uint32_t task_index)
{
    cpu_core_e core = curr_core();

    /* New trial? Drop this task's tracking so its first instance is not measured against
     * the prior trial (whose last instance kill_thread cut short). */
    if (g_task_trial[core][task_index] != g_edf_u_index)
    {
        g_task_trial[core][task_index]   = g_edf_u_index;
        g_task_started[core][task_index] = 0u;
    }

    if (g_task_started[core][task_index] &&
        g_task_completions[core][task_index] == g_task_start_snap[core][task_index])
    {
        raise_error_ctx("edf watchdog: task re-run without completing prior instance (premature FINISH)",
                        (uint32_t)(uintptr_t)g_edf_handles[core][task_index]);
    }

    g_task_started[core][task_index]    = 1u;
    g_task_start_snap[core][task_index] = g_task_completions[core][task_index];

    do_work(task_iters[task_index]);

    if (core == CPU0) g_edf_done[task_index]++;

    g_task_completions[core][task_index]++;   /* the one legitimate finish signal */
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


/* Record the corruption for the host, then trap. raise_error_ctx does not return. */
static void edf_flag_corruption(const char* message, thread_t* thread, cpu_core_e core,
                                uint32_t task_index, uint32_t expected, uint32_t actual)
{
    g_edf_bad_thread   = (uint32_t)(uintptr_t)thread;
    g_edf_bad_core     = (uint32_t)core;
    g_edf_bad_task     = task_index;
    g_edf_bad_expected = expected;
    g_edf_bad_actual   = actual;
    g_edf_bad_tick     = g_cpus[core].ticks;

    __asm__ volatile("dmb sy" ::: "memory");
    raise_error_ctx(message, (uint32_t)(uintptr_t)thread);
}


/* Validate one live task against the values add_periodic_thread_to_core stamped. Fields
 * period/func/periodicity/core are set once and never change; a live task must not have
 * been removed from the scheduler; sp must live inside the task's own stack. Any deviation
 * means the struct got clobbered (freed + reused, stray write, etc.). */
static void edf_check_task(cpu_core_e core, uint32_t task_index)
{
    thread_t* thread = g_edf_handles[core][task_index];
    if (thread == NULL) return;

    if (thread->md.periodicity != PERIODIC)
        edf_flag_corruption("edf watchdog: task periodicity corrupted",
                            thread, core, task_index, (uint32_t)PERIODIC, (uint32_t)thread->md.periodicity);

    if (thread->timing.period != g_edf_periods[task_index])
        edf_flag_corruption("edf watchdog: task period corrupted",
                            thread, core, task_index, g_edf_periods[task_index], thread->timing.period);

    if (thread->func != JOBS[task_index])
        edf_flag_corruption("edf watchdog: task func corrupted",
                            thread, core, task_index, (uint32_t)(uintptr_t)JOBS[task_index], (uint32_t)(uintptr_t)thread->func);

    if (thread->core != core)
        edf_flag_corruption("edf watchdog: task core corrupted",
                            thread, core, task_index, (uint32_t)core, (uint32_t)thread->core);

    if (thread->md.status == REMOVED)
        edf_flag_corruption("edf watchdog: live task was removed from the scheduler",
                            thread, core, task_index, (uint32_t)REMOVED, (uint32_t)thread->md.status);

    if ((char*)thread->sp < thread->stack || (char*)thread->sp > thread->stack + THREAD_STACK_SIZE)
        edf_flag_corruption("edf watchdog: task sp outside its stack",
                            thread, core, task_index, (uint32_t)(uintptr_t)thread->stack, (uint32_t)(uintptr_t)thread->sp);
}


void ktrace_edf_tick(thread_t* running)
{
    if (!trace_active) return;

    cpu_core_e core = curr_core();
    uint32_t running_id = running->id;

    if (g_trace_len[core] < TRACE_TICKS)
    {
        g_sched_trace[core][g_trace_len[core]++] = (uint8_t)running_id;
    }

    if (core == CPU0 && running_id < NTASKS)
    {
        g_edf_metrics[running_id] = running->metrics;
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


// Calculates avg number of cycles per tick
static uint32_t measure_cycles_per_tick(void)
{
    uint32_t tick = rd_ticks();

    /* Synchronize to a tick edge. */
    while (rd_ticks() == tick){}

    uint32_t start = pmu_cycles();
    tick = rd_ticks();

    while (rd_ticks() < tick + 16u){}   // 16 tick averaging window

    return (pmu_cycles() - start) / 16u;
}


// Runs the calibration burst and returns cycles per do_work iteration (>= 1)
static uint32_t measure_cycles_per_iter(void)
{
    __asm__ __volatile__(
        "cpsid " PREEMPT_MASK
        :
        :
        : "memory"
    );


    uint32_t start = pmu_cycles();

    do_work(CALIB_ITERS);

    uint32_t cycles = pmu_cycles() - start;


    __asm__ __volatile__(
        "cpsie " PREEMPT_MASK
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
        uint32_t compute_ticks = (u_permille * g_edf_periods[i]) / (1000u * NTASKS);

        if (compute_ticks < 1u) compute_ticks = 1u;

        g_edf_C[i]    = compute_ticks;
        task_iters[i] = (uint32_t)((uint64_t)compute_ticks * cycles_per_tick / cycles_per_iter);
    }
}


static void reset_trial(void)
{
    g_cpus[curr_core()].ticks = 0u;
    g_cpus[curr_core()].missed_deadlines = 0u;


    trace_active = 1u;   /* trace every trial; the host reads each one and picks which to plot */

    for (uint32_t core = 0; core < NUM_CPUS; core++)
    {
        g_trace_len[core]                   = 0u;
        g_cpus[core].avg_scheduler_overhead = 0u;   /* fresh per-CPU scheduler-overhead EWMA per trial */
    }


    for (uint32_t i = 0; i < NTASKS; i++)
    {
        g_edf_done[i]    = 0u;
        g_edf_metrics[i] = (thread_metrics_t){0};
    }
}


/* -------------------------------------------------------------------------
 * Benchmark
 * ------------------------------------------------------------------------- */

void edf_run(void)
{
    pmu_init();

    /* allocbench destroyed its heap before returning. */
    heap_init();
    psched_init();
    __asm__ __volatile__(
            "cpsie " PREEMPT_MASK
            :
            :
            : "memory"
    );

    for (uint32_t core = 0; core < NUM_CPUS; core++)
    {
        g_ticks_m[core]    = 0u;
        g_misses_m[core]   = 0u;
        g_overhead_m[core] = 0u;
        g_core_u_m[core]   = 0u;
    }

    for (uint32_t i = 0; i < NTASKS; i++)
    {
        g_edf_metrics[i] = (thread_metrics_t){0};
    }


    uint32_t cycles_per_tick = measure_cycles_per_tick();
    uint32_t cycles_per_iter = measure_cycles_per_iter();


    for (uint32_t trial = 0; trial < g_edf_u_count; trial++)
    {
        /* Trial construction must not race the scheduler. */
        __asm__ __volatile__(
            "cpsid " PREEMPT_MASK
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

        /* Both cores run the same workload concurrently. The allocator mutex makes the
         * shared heap safe, and each core's core_mutex makes the cross-core push into
         * CPU1's incoming FIFO safe against CPU1's own scheduler. */
        for (uint32_t i = 0; i < NTASKS; i++)
        {
            g_edf_handles[CPU0][i] = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, JOBS[i], g_edf_periods[i], PERIODIC, i);
#if ENABLE_SMP
            g_edf_handles[CPU1][i] = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU1, JOBS[i], g_edf_periods[i], PERIODIC, i);
#endif
        }

        g_edf_armed = 1u;   /* task set complete -- watchdog live from here */

        __asm__ __volatile__(
            "dmb sy"
            :
            :
            : "memory"
        );


        /* Python has a hardware breakpoint installed here. Reaching it means this trial
         * is fully configured. */
        KTRACE_EDF_READY();

        __asm__ __volatile__(
            "cpsie " PREEMPT_MASK
            :
            :
            : "memory"
        );


        /* Main stays here while scheduler IRQs execute the EDF workload. Python resumes
         * after the breakpoint, waits 12 seconds, halts, samples state, writes
         * g_test_release = 1, resumes. */
        KTRACE_WAIT_RELEASE();


        /* Remove this trial before constructing the next one. */
        __asm__ __volatile__(
            "cpsid " PREEMPT_MASK
            :
            :
            : "memory"
        );


        g_edf_armed = 0u;   /* tearing the trial down -- stop validating (kill flips fields) */

        // psched_clear_threads();
        for (uint32_t i = 0; i < NTASKS; i++)
        {
            kill_thread(g_edf_handles[CPU0][i]);
#if ENABLE_SMP
            kill_thread(g_edf_handles[CPU1][i]);
#endif
        }


        __asm__ __volatile__(
            "cpsie " PREEMPT_MASK
            :
            :
            : "memory"
        );

        /* kill_thread already pulled them from the heaps; wait a few ticks for each core to
         * switch off any killed curr_thread, then free this trial's storage. */
        uint32_t reap_start_tick = g_cpus[CPU0].ticks;
        while ((g_cpus[CPU0].ticks - reap_start_tick) < 8u) { __asm__ __volatile__("" ::: "memory"); }
        for (uint32_t i = 0; i < NTASKS; i++)
        {
            if (g_edf_handles[CPU0][i]) kFree(g_edf_handles[CPU0][i]);
#if ENABLE_SMP
            if (g_edf_handles[CPU1][i]) kFree(g_edf_handles[CPU1][i]);
#endif
        }
    }


    /* Sweep complete on both cores -- tear the scheduler back down (CPU0 locally + IPI to
     * CPU1), mirroring the psched_init() at the top. */
    psched_deinit();

    KTRACE_EDF_DONE();
}



#if ENABLE_SMP   /* load-balance is intrinsically cross-core: nothing to balance on one core */

/* -------------------------------------------------------------------------
 * Load-balance test: six equal u=0.25 periodic tasks (total 1.5) handed to the balancer
 * (add_periodic_thread), which should spread them to ~0.75 on each core. Self-contained:
 * brings the scheduler up, runs, kills its tasks, tears back down. Tasks are labelled by
 * id; add_periodic_thread's return tells us which core each landed on.
 * ------------------------------------------------------------------------- */

#define BAL_NTASKS   6u

const uint32_t g_bal_periods[BAL_NTASKS] = { 40u, 40u, 60u, 60u, 100u, 100u };

SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_bal_assign[BAL_NTASKS];   /* id -> assigned core  */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_bal_done[BAL_NTASKS];     /* per-task completions */

static uint32_t bal_iters[BAL_NTASKS];


/* One body for every balance task; each reads its own id from curr_thread. */
static sys_exit_e bal_job(void)
{
    uint32_t task_id = g_cpus[curr_core()].curr_thread->id;

    do_work(bal_iters[task_id]);

    g_bal_done[task_id]++;
    return SYS_OK;
}


void edf_balance_run(void)
{
    pmu_init();
    heap_init();            /* guarded: no-op if already live */
    psched_init();

    for (uint32_t core = 0; core < NUM_CPUS; core++)
    {
        g_trace_len[core] = 0u;
        g_core_u_m[core]  = 0u;
    }

    uint32_t cycles_per_tick = measure_cycles_per_tick();
    uint32_t cycles_per_iter = measure_cycles_per_iter();

    for (uint32_t i = 0; i < BAL_NTASKS; i++)
    {
        uint32_t compute_ticks = g_bal_periods[i] / 4u;   /* u = 0.25 -> Ci = period/4 */
        if (compute_ticks < 1u) compute_ticks = 1u;

        bal_iters[i]  = (uint32_t)((uint64_t)compute_ticks * cycles_per_tick / cycles_per_iter);
        g_bal_done[i] = 0u;
    }

    g_test_release = 0u;

    thread_t* handles[BAL_NTASKS];

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    trace_active = 1u;

    for (uint32_t i = 0; i < BAL_NTASKS; i++)
    {
        handles[i] = add_periodic_thread(kMalloc(sizeof(thread_t)), bal_job, g_bal_periods[i], PERIODIC, i);
        g_bal_assign[i] = (uint32_t)handles[i]->core;
    }

    __asm__ __volatile__("dmb sy" ::: "memory");

    KTRACE_BALANCE_READY();

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    KTRACE_WAIT_RELEASE();

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");

    for (uint32_t i = 0; i < BAL_NTASKS; i++)
    {
        kill_thread(handles[i]);
    }

    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    psched_deinit();

    for (uint32_t i = 0; i < BAL_NTASKS; i++) if (handles[i]) kFree(handles[i]);

    KTRACE_BALANCE_DONE();
}

#endif  /* ENABLE_SMP (load-balance test) */
