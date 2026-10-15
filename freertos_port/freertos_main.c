/* FreeRTOS battery entry (FREERTOS=1). The FreeRTOS counterpart to the Kairos main():
 * bring up the scheduler and run every bench as a FreeRTOS task on the live scheduler.
 * The bench task runs the same allocator sweeps as the Kairos battery (heap_4 instead of
 * TLSF) plus the FreeRTOS scheduler-cost sweep, hitting the same ktrace breakpoints so
 * test_freertos.py reads it exactly like test.py reads the Kairos battery.
 * See freertos_port/PLAN.md. */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "ktrace.h"

extern void allocsweep_run( void );            /* heap_4 alloc/free cost vs payload size */
extern void fragsweep_run( void );             /* heap_4 alloc/free cost vs free-list length */
extern void freertos_schedsweep_run( void );   /* scheduler cost vs loaded task count */


/* The CA9 port saves/restores VFP registers on a context switch, so the FPU must be on
 * before any task runs even though nothing uses floating point. */
static void prvEnableFPU( void )
{
    __asm__ volatile (
        "mrc p15, 0, r0, c1, c0, 2   \n"   /* CPACR */
        "orr r0, r0, #(0xF << 20)    \n"   /* full access to cp10 + cp11 */
        "mcr p15, 0, r0, c1, c0, 2   \n"
        "isb                          \n"
        "mov r0, #0x40000000         \n"
        "vmsr fpexc, r0              \n"   /* FPEXC.EN = 1 */
        ::: "r0", "memory" );
}


static void prvBenchTask( void* pvParameters )
{
    ( void ) pvParameters;

    allocsweep_run();
    fragsweep_run();
    KTRACE_ALLOC_DONE();

    freertos_schedsweep_run();
    KTRACE_SCHED_SWEEP_DONE();

    for( ; ; )
    {
        vTaskDelay( portMAX_DELAY );
    }
}


void freertos_main( void )
{
    prvEnableFPU();

    xTaskCreate( prvBenchTask, "bench", configMINIMAL_STACK_SIZE * 4, NULL,
                 configMAX_PRIORITIES - 1, NULL );

    vTaskStartScheduler();

    /* Only reached if the scheduler could not be started (out of heap). */
    for( ; ; )
    {
    }
}
