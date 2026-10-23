/* Scheduler overhead as a function of the number of loaded tasks (single core, CPU0).
 * Underloaded on purpose (each task does ~no work) so the curve isolates the per-tick
 * scheduler cost -- the O(log N) heap work in next_up -- rather than deadline-miss
 * churn. Per N we capture the full distribution of per-tick next_thread cost, read out
 * as memlat-style range bars by test.py. See freertos_port/PLAN.md. */

#include <stdint.h>

#include "cpu.h"
#include "ktrace.h"
#include "pmu.h"
#include "sched.h"
#include "telemetry.h"
#include "thread.h"
#include "tlsf.h"


#define SCHED_SAMPLES   512u            /* per-tick overhead samples captured per level */
#define SCHED_MAX_TASKS 64u


/* Task-count levels swept. */
static const uint32_t sched_levels[] =
{
    1u, 2u, 4u, 8u, 16u, 32u, 64u
};

#define SCHED_N_LEVELS  ( sizeof( sched_levels ) / sizeof( sched_levels[ 0 ] ) )


/* Host-readable: the x-axis (task counts) and, per level, the raw per-tick scheduler
 * overhead samples (cycles). test.py reduces each row to min/mean/p50/p99/max. */
HOST_SHARED uint32_t g_schedsweep_levels[ SCHED_N_LEVELS ];
HOST_SHARED uint32_t g_schedsweep_n;
HOST_SHARED uint32_t g_schedsweep_samples[ SCHED_N_LEVELS ][ SCHED_SAMPLES ];
HOST_SHARED volatile uint32_t g_schedsweep_counts[ SCHED_N_LEVELS ];

static volatile uint32_t sweep_active = 0u;
static volatile uint32_t sweep_level  = 0u;


/* Called from the per-tick scheduler-cost hook (ktrace_sched_cost). Returns 1 when it
 * consumes the sample (schedsweep active), so the ctxsw micro-bench does not also count
 * it. CPU0 only, for the single-core measurement. */
int schedsweep_sample( uint32_t cost )
{
    if( sweep_active == 0u )
    {
        return 0;
    }
    if( curr_core() != CPU0 )
    {
        return 1;
    }

    uint32_t L = sweep_level;
    uint32_t c = g_schedsweep_counts[ L ];
    if( c < SCHED_SAMPLES )
    {
        g_schedsweep_samples[ L ][ c ] = cost;
        g_schedsweep_counts[ L ] = c + 1u;
    }
    return 1;
}


/* Minimal periodic job: returns immediately, so the taskset stays far below capacity. */
static sys_exit_e sched_job( void )
{
    return SYS_OK;
}


void schedsweep_run( void )
{
    pmu_init();
    heap_init();
    psched_init();
    __asm__ volatile ( "cpsie " PREEMPT_MASK ::: "memory" );

    g_schedsweep_n = ( uint32_t ) SCHED_N_LEVELS;

    thread_t* handles[ SCHED_MAX_TASKS ];

    for( uint32_t li = 0; li < SCHED_N_LEVELS; li++ )
    {
        uint32_t N = sched_levels[ li ];
        g_schedsweep_levels[ li ] = N;
        g_schedsweep_counts[ li ] = 0u;

        /* Load N underloaded periodic tasks on CPU0 (spread periods -> staggered
         * releases, representative of a real taskset rather than a synchronized storm;
         * the ~no-op job keeps compute utilization near zero). */
        __asm__ volatile ( "cpsid " PREEMPT_MASK ::: "memory" );
        for( uint32_t i = 0; i < N; i++ )
        {
            handles[ i ] = add_periodic_thread_to_core( kMalloc(sizeof(thread_t)), CPU0, sched_job, 100u + i, PERIODIC, i );
        }
        sweep_level = li;
        __asm__ volatile ( "dmb sy" ::: "memory" );
        sweep_active = 1u;
        __asm__ volatile ( "cpsie " PREEMPT_MASK ::: "memory" );

        /* Spin (in main_thread) while CPU0's scheduler ticks fill this level's buffer.
         * Underloaded, so CPU0 is mostly idle and main_thread runs to observe progress. */
        while( g_schedsweep_counts[ li ] < SCHED_SAMPLES )
        {
            __asm__ volatile ( "" ::: "memory" );
        }

        sweep_active = 0u;
        __asm__ volatile ( "dmb sy" ::: "memory" );

        __asm__ volatile ( "cpsid " PREEMPT_MASK ::: "memory" );
        for( uint32_t i = 0; i < N; i++ )
        {
            kill_thread( handles[ i ] );
        }
        __asm__ volatile ( "cpsie " PREEMPT_MASK ::: "memory" );

        /* Let the killed tasks reap (next_up drains them) before the next level. */
        uint32_t t0 = g_cpus[ CPU0 ].ticks;
        while( ( g_cpus[ CPU0 ].ticks - t0 ) < 8u )
        {
            __asm__ volatile ( "" ::: "memory" );
        }

        for( uint32_t i = 0; i < N; i++ )
        {
            if( handles[ i ] ) kFree( handles[ i ] );
        }
    }

    psched_deinit();

    KTRACE_SCHED_SWEEP_DONE();
}
