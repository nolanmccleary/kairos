/* Allocator-path glue (step 1). Lets FreeRTOS heap_4.c link and run WITHOUT the
 * scheduler, so TLSF and heap_4 can be benchmarked head-to-head in one build.
 * See freertos_port/PLAN.md. */

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

/* configAPPLICATION_ALLOCATED_HEAP == 1: heap_4.c uses `extern uint8_t ucHeap[...]`.
 * OCRAM is only 64 KB, so the heap_4 region lives in SDRAM. The linker places
 * .freertos_heap in SDRAM as NOLOAD (added in de1-soc.ld, step 1 milestone B). */
__attribute__( ( section( ".freertos_heap" ), aligned( 8 ) ) )
uint8_t ucHeap[ configTOTAL_HEAP_SIZE ];

/* heap_4 brackets its critical sections with the scheduler's suspend/resume. In the
 * allocator-only build there is no scheduler, so reproduce FreeRTOS's own pre-scheduler
 * behavior: a nesting counter, no context switch. Defined here only when the real
 * scheduler (tasks.c) is NOT linked; the scheduler build (USE_FREERTOS) uses the real
 * ones from tasks.c instead. */
#ifndef USE_FREERTOS_SCHED

static volatile UBaseType_t uxHeapSuspendNesting = 0;

void vTaskSuspendAll( void )
{
    uxHeapSuspendNesting++;
}

BaseType_t xTaskResumeAll( void )
{
    if( uxHeapSuspendNesting > 0 )
    {
        uxHeapSuspendNesting--;
    }
    return pdFALSE;
}

/* heap_4 also brackets some updates with taskENTER_CRITICAL and an ISR interrupt
 * mask. In the allocator-only build there is one core and no scheduler, so a CPSR
 * interrupt mask gives the mutual exclusion heap_4 expects. The full scheduler build
 * (USE_FREERTOS) uses port.c's real GIC-priority-mask versions instead, so the cost
 * there reflects the production critical section; here it is a lighter CPSR mask. */
static volatile uint32_t uxCriticalNesting = 0;

void vPortEnterCritical( void )
{
    __asm__ volatile ( "cpsid i" ::: "memory" );
    uxCriticalNesting++;
}

void vPortExitCritical( void )
{
    if( uxCriticalNesting > 0 )
    {
        if( --uxCriticalNesting == 0 )
        {
            __asm__ volatile ( "cpsie i" ::: "memory" );
        }
    }
}

uint32_t ulPortSetInterruptMask( void )
{
    uint32_t ulPrev;
    __asm__ volatile ( "mrs %0, cpsr\n\t"
                       "cpsid i" : "=r"( ulPrev ) :: "memory" );
    return ulPrev;
}

void vPortClearInterruptMask( uint32_t ulNewMaskValue )
{
    if( ulNewMaskValue == 0 )
    {
        __asm__ volatile ( "cpsie i" ::: "memory" );   /* portENABLE_INTERRUPTS */
    }
    else
    {
        __asm__ volatile ( "msr cpsr_c, %0" :: "r"( ulNewMaskValue ) : "memory" );
    }
}

#endif /* USE_FREERTOS_SCHED */
