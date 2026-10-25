/* FreeRTOS scheduler overhead as a function of the number of loaded tasks (single core).
 * The FreeRTOS counterpart to workload_schedsweep.c: underloaded on purpose (each task
 * just sleeps) so the curve isolates the per-tick scheduler cost -- here the fixed-priority
 * xTaskIncrementTick (delayed-list servicing + the O(1) ready-list switch decision), which
 * is expected flat in N where the Kairos EDF next_up is O(log N). Per N we capture the full
 * distribution of per-tick cost, read out as the same range bars test.py uses for Kairos.
 * See freertos_port/PLAN.md. */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "telemetry.h"


#define SCHED_SAMPLES   512u            /* per-tick overhead samples captured per level */
#define SCHED_MAX_TASKS 64u


/* Task-count levels swept (mirror the Kairos schedsweep). */
static const uint32_t sched_levels[] =
{
    1u, 2u, 4u, 8u, 16u, 32u, 64u
};

#define SCHED_N_LEVELS  ( sizeof( sched_levels ) / sizeof( sched_levels[ 0 ] ) )


/* Host-readable: the x-axis (task counts) and, per level, the raw per-tick scheduler
 * overhead samples (cycles). test_freertos.py reduces each row to min/mean/p50/p99/max.
 * Distinct names from the Kairos g_schedsweep_* so both workloads can coexist in CORE. */
SDRAM_UNCACHED_TELEMETRY uint32_t g_frtos_sched_levels[ SCHED_N_LEVELS ];
SDRAM_UNCACHED_TELEMETRY uint32_t g_frtos_sched_n;
SDRAM_UNCACHED_TELEMETRY uint32_t g_frtos_sched_samples[ SCHED_N_LEVELS ][ SCHED_SAMPLES ];
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_frtos_sched_counts[ SCHED_N_LEVELS ];

static volatile uint32_t sweep_active = 0u;
static volatile uint32_t sweep_level  = 0u;


/* Called from the glue's tick path (vApplicationIRQHandler) with the measured cost of
 * FreeRTOS_Tick_Handler. Returns 1 when it consumes the sample (sweep active). Single
 * core, so no core check: CPU1 is parked and never takes this tick. */
int freertos_schedsweep_sample( uint32_t cost )
{
    if( sweep_active == 0u )
    {
        return 0;
    }

    uint32_t L = sweep_level;
    uint32_t c = g_frtos_sched_counts[ L ];
    if( c < SCHED_SAMPLES )
    {
        g_frtos_sched_samples[ L ][ c ] = cost;
        g_frtos_sched_counts[ L ] = c + 1u;
    }
    return 1;
}


/* Minimal load task: sleeps a per-task period, so it sits in the delayed list and the
 * taskset stays far below capacity. The period (spread across tasks) is passed as pv. */
static void load_task( void* pvParameters )
{
    TickType_t xPeriod = ( TickType_t ) ( uintptr_t ) pvParameters;
    for( ; ; )
    {
        vTaskDelay( xPeriod );
    }
}


void freertos_schedsweep_run( void )
{
    g_frtos_sched_n = ( uint32_t ) SCHED_N_LEVELS;

    TaskHandle_t handles[ SCHED_MAX_TASKS ];

    for( uint32_t li = 0; li < SCHED_N_LEVELS; li++ )
    {
        uint32_t N = sched_levels[ li ];
        g_frtos_sched_levels[ li ] = N;
        g_frtos_sched_counts[ li ] = 0u;

        /* Load N underloaded tasks with spread periods (100..100+N ticks) so releases
         * stagger rather than storm, mirroring the Kairos schedsweep taskset. */
        for( uint32_t i = 0; i < N; i++ )
        {
            xTaskCreate( load_task, "load", configMINIMAL_STACK_SIZE,
                         ( void* ) ( uintptr_t ) ( 100u + i ),
                         tskIDLE_PRIORITY + 1, &handles[ i ] );
        }

        /* Let the load tasks run once and settle into the delayed list before sampling. */
        vTaskDelay( 16 );

        sweep_level = li;
        __asm__ volatile ( "dmb sy" ::: "memory" );
        sweep_active = 1u;

        /* Sleep a tick at a time while the per-tick hook fills this level's buffer. */
        while( g_frtos_sched_counts[ li ] < SCHED_SAMPLES )
        {
            vTaskDelay( 1 );
        }

        sweep_active = 0u;
        __asm__ volatile ( "dmb sy" ::: "memory" );

        for( uint32_t i = 0; i < N; i++ )
        {
            vTaskDelete( handles[ i ] );
        }

        /* Let the idle task reclaim the deleted tasks before the next level. */
        vTaskDelay( 16 );
    }
}
