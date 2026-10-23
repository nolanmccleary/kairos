#include <stdint.h>

#include "cpu.h"
#include "fault.h"
#include "ktrace.h"
#include "pmu.h"
#include "sched.h"
#include "telemetry.h"
#include "thread.h"
#include "tlsf.h"


#ifdef MODE_TEST


/* -------------------------------------------------------------------------
 * suspend_thread() test. Two self-contained scenarios, both pinned to CPU0 so
 * the mechanism is exercised deterministically (SMP cross-core suspend is a
 * separate concern). Each scenario brings tasks up on the already-running
 * scheduler, runs until its success condition (or a seconds-scale budget cap so
 * a broken suspend can never hang the suite), then kills its tasks.
 *
 *   A (self-suspend): the novel, dangerous path -- a running task suspends
 *     ITSELF, forcing the synchronous context switch (the __asm__ goto frame
 *     build + SWITCH_FRAME_AND_RETURN restore). We verify it (1) actually stops
 *     and comes back, (2) resumes with its callee-saved register file intact
 *     (the r4-r11-drop bug class), and (3) the gap is exactly the requested
 *     delay. release_time = T+delay and a low-load core re-selects it the tick
 *     ticks reaches T+delay, so measured_delay == delay (give or take a tick of
 *     contention).
 *
 *   B (suspend-another): a controller task suspends a DIFFERENT victim task that
 *     is not currently running (the non-asm branch). We verify the victim
 *     disappears from the schedule for ~delay ticks, then runs again.
 *
 * All host-readable state lives in .telemetry (HOST_SHARED); the whole file is
 * MODE_TEST-only.
 * ------------------------------------------------------------------------- */

#define SELF_PERIOD              6u
#define SELF_DELAY               8u
#define SELF_SUSPEND_ACTIVATION  3u    /* suspend on the 4th activation (0-indexed) */
#define SELF_TARGET_RUNS         10u   /* stop once it has run this many times (proves it kept running) */

#define OTHER_VICTIM_PERIOD      4u
#define OTHER_CTRL_PERIOD        5u
#define OTHER_DELAY              12u
#define CTRL_SUSPEND_ACTIVATION  2u    /* controller suspends the victim on its 3rd activation */
#define OTHER_TARGET_RUNS        16u   /* victim runs to collect before/after the suspension gap */

#define SUSP_TICKS               64u   /* per-scenario run-tick capture depth (host plots these) */
#define SUSP_BUDGET              1000000000u  /* hard spin cap: ~seconds, so a stuck suspend can't hang forever */

#define SELF_ID                  0u
#define VICTIM_ID                1u
#define CTRL_ID                  2u


/* Requested delays, published for the host to read back. Written at runtime (not
 * const-initialized) so they live in the KEEP'd .telemetry region and survive
 * --gc-sections even though the firmware itself uses the SELF_DELAY/OTHER_DELAY
 * macros directly. */
HOST_SHARED volatile uint32_t g_susp_self_requested_delay;
HOST_SHARED volatile uint32_t g_susp_other_requested_delay;


/* --- scenario A (self-suspend) host-readable state ------------------------- */
HOST_SHARED volatile uint32_t g_susp_self_ticks[SUSP_TICKS];  /* tick of each activation */
HOST_SHARED volatile uint32_t g_susp_self_n;                  /* activation count (also capture index) */
HOST_SHARED volatile uint32_t g_susp_self_delay;              /* measured ticks the task was gone */
HOST_SHARED volatile uint32_t g_susp_self_resumed;            /* 1 once it returns from suspend_thread */
HOST_SHARED volatile uint32_t g_susp_self_regs_ok;            /* 1 if callee-saved sentinels survived resume */
HOST_SHARED volatile uint32_t g_susp_self_done;               /* scenario-complete flag the driver spins on */


/* --- scenario B (suspend-another) host-readable state ---------------------- */
HOST_SHARED volatile uint32_t g_susp_other_ticks[SUSP_TICKS]; /* tick of each victim activation */
HOST_SHARED volatile uint32_t g_susp_other_n;                 /* victim activation count */
HOST_SHARED volatile uint32_t g_susp_other_suspend_tick;      /* tick the controller issued the suspend */
HOST_SHARED volatile uint32_t g_susp_other_last_pre;          /* last victim run tick before the suspend */
HOST_SHARED volatile uint32_t g_susp_other_first_post;        /* first victim run tick after the suspend */
HOST_SHARED volatile uint32_t g_susp_other_armed;             /* 1 once the suspend has been issued */
HOST_SHARED volatile uint32_t g_susp_other_done;              /* scenario-complete flag */


/* Progress marker (host polls to localize a stall): 1=self 2=other 3=done. */
HOST_SHARED volatile uint32_t g_susp_phase;


static thread_t*         g_susp_victim;   /* victim handle, read by the controller job */
static volatile uint32_t susp_ctrl_n;     /* controller activation counter */


/* -------------------------------------------------------------------------
 * Scenario A: suspender suspends itself.
 * ------------------------------------------------------------------------- */
static sys_exit_e suspender_job(void)
{
    cpu_core_e core = curr_core();
    uint32_t t = (uint32_t)g_cpus[core].ticks;

    uint32_t idx = g_susp_self_n;
    if (idx < SUSP_TICKS) g_susp_self_ticks[idx] = t;
    g_susp_self_n = idx + 1u;

    if (idx == SELF_SUSPEND_ACTIVATION)
    {
        thread_t* self = g_cpus[core].curr_thread;

        /* Sentinels held live across the suspend call. If the frame build or the
         * IRQ-path restore drops r4-r11 (the historical bug), these won't match. */
        volatile uint32_t seed = idx;   /* volatile source -> not constant-folded away */
        uint32_t s0 = 0xA5A50000u + seed;
        uint32_t s1 = 0x5A5A1111u + seed;
        uint32_t s2 = 0xC3C32222u + seed;
        uint32_t s3 = 0x3C3C3333u + seed;

        uint32_t t_before = (uint32_t)g_cpus[core].ticks;

        suspend_thread(self, SELF_DELAY);

        uint32_t t_after = (uint32_t)g_cpus[core].ticks;

        uint32_t ok = (s0 == 0xA5A50000u + seed) && (s1 == 0x5A5A1111u + seed)
                   && (s2 == 0xC3C32222u + seed) && (s3 == 0x3C3C3333u + seed);

        g_susp_self_delay   = t_after - t_before;
        g_susp_self_regs_ok = ok ? 1u : 0u;
        g_susp_self_resumed = 1u;
    }

    if (g_susp_self_n >= SELF_TARGET_RUNS) g_susp_self_done = 1u;

    return SYS_OK;
}


/* -------------------------------------------------------------------------
 * Scenario B: controller suspends the victim (not the running task).
 * ------------------------------------------------------------------------- */
static sys_exit_e victim_job(void)
{
    cpu_core_e core = curr_core();
    uint32_t t = (uint32_t)g_cpus[core].ticks;

    uint32_t idx = g_susp_other_n;
    if (idx < SUSP_TICKS) g_susp_other_ticks[idx] = t;
    g_susp_other_n = idx + 1u;

    if (!g_susp_other_armed)                    g_susp_other_last_pre   = t;
    else if (g_susp_other_first_post == 0u)     g_susp_other_first_post = t;

    return SYS_OK;
}


static sys_exit_e controller_job(void)
{
    cpu_core_e core = curr_core();
    uint32_t n = susp_ctrl_n++;

    if (n == CTRL_SUSPEND_ACTIVATION && !g_susp_other_armed)
    {
        g_susp_other_suspend_tick = (uint32_t)g_cpus[core].ticks;
        suspend_thread(g_susp_victim, OTHER_DELAY);
        g_susp_other_armed = 1u;
    }

    if (g_susp_other_armed && g_susp_other_first_post != 0u && g_susp_other_n >= OTHER_TARGET_RUNS)
    {
        g_susp_other_done = 1u;
    }

    return SYS_OK;
}


/* -------------------------------------------------------------------------
 * Driver
 * ------------------------------------------------------------------------- */
void suspend_run(void)
{
    pmu_init();
    heap_init();
    psched_init();

    g_susp_self_requested_delay  = SELF_DELAY;
    g_susp_other_requested_delay = OTHER_DELAY;

    /* ---- scenario A ---- */
    g_susp_phase        = 1u;
    g_susp_self_n       = 0u;
    g_susp_self_delay   = 0u;
    g_susp_self_resumed = 0u;
    g_susp_self_regs_ok = 0u;
    g_susp_self_done    = 0u;
    for (uint32_t i = 0; i < SUSP_TICKS; i++) g_susp_self_ticks[i] = 0u;

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");
    thread_t* suspender = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, suspender_job, SELF_PERIOD, PERIODIC, SELF_ID);
    if (suspender == NULL) raise_error("suspend_run: could not create suspender");
    __asm__ __volatile__("dmb sy" ::: "memory");
    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    uint32_t budget = 0u;
    while (!g_susp_self_done && budget < SUSP_BUDGET) { budget++; __asm__ __volatile__("" ::: "memory"); }

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");
    kill_thread(suspender);
    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    /* ---- scenario B ---- */
    g_susp_phase              = 2u;
    g_susp_other_n            = 0u;
    g_susp_other_suspend_tick = 0u;
    g_susp_other_last_pre     = 0u;
    g_susp_other_first_post   = 0u;
    g_susp_other_armed        = 0u;
    g_susp_other_done         = 0u;
    susp_ctrl_n               = 0u;
    for (uint32_t i = 0; i < SUSP_TICKS; i++) g_susp_other_ticks[i] = 0u;

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");
    g_susp_victim = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, victim_job, OTHER_VICTIM_PERIOD, PERIODIC, VICTIM_ID);
    thread_t* controller = add_periodic_thread_to_core(kMalloc(sizeof(thread_t)), CPU0, controller_job, OTHER_CTRL_PERIOD, PERIODIC, CTRL_ID);
    if (g_susp_victim == NULL || controller == NULL) raise_error("suspend_run: could not create scenario-B tasks");
    __asm__ __volatile__("dmb sy" ::: "memory");
    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    budget = 0u;
    while (!g_susp_other_done && budget < SUSP_BUDGET) { budget++; __asm__ __volatile__("" ::: "memory"); }

    __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");
    kill_thread(g_susp_victim);
    kill_thread(controller);
    __asm__ __volatile__("cpsie " PREEMPT_MASK ::: "memory");

    psched_deinit();

    if (suspender)     kFree(suspender);
    if (controller)    kFree(controller);
    if (g_susp_victim) kFree(g_susp_victim);

    g_susp_phase = 3u;
    KTRACE_SUSPEND_DONE();
}


#endif  /* MODE_TEST */
