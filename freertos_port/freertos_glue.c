/* FreeRTOS scheduler glue (USE_FREERTOS_SCHED builds). Connects the vendored CA9 port
 * to the DE1 hardware the Kairos boot already brought up:
 *   - vConfigureTickInterrupt / vClearTickInterrupt: arm + clear the A9 global timer at
 *     configTICK_RATE_HZ and enable its GIC interrupt (PPI 27).
 *   - vApplicationIRQHandler: dispatch by interrupt ID (the port reads IAR / writes EOIR
 *     around this call); the tick ID goes to FreeRTOS_Tick_Handler.
 * See freertos_port/PLAN.md. */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "board.h"   /* BOARD_GTIMER_BASE / BOARD_GICD_BASE */
#include "pmu.h"     /* pmu_cycles() */

/* A9 global timer (shared counter + per-core comparator), same block Kairos uses. */
#define GT_CNTL    ( *( volatile uint32_t* )( BOARD_GTIMER_BASE + 0x00u ) )
#define GT_CNTH    ( *( volatile uint32_t* )( BOARD_GTIMER_BASE + 0x04u ) )
#define GT_CTRL    ( *( volatile uint32_t* )( BOARD_GTIMER_BASE + 0x08u ) )
#define GT_ISR     ( *( volatile uint32_t* )( BOARD_GTIMER_BASE + 0x0Cu ) )
#define GT_CMPL    ( *( volatile uint32_t* )( BOARD_GTIMER_BASE + 0x10u ) )
#define GT_CMPH    ( *( volatile uint32_t* )( BOARD_GTIMER_BASE + 0x14u ) )
#define GT_AUTOINC ( *( volatile uint32_t* )( BOARD_GTIMER_BASE + 0x18u ) )

#define GICD_ISENABLER0 ( *( volatile uint32_t* )( BOARD_GICD_BASE + 0x100u ) )
#define GICD_IPRIORITY  ( ( volatile uint8_t*  )( BOARD_GICD_BASE + 0x400u ) )

#define TICK_INT_ID   27u        /* A9 global timer PPI */
#define GT_RELOAD     199999u    /* periph clock 200 MHz / 1000 Hz - 1 == 1 kHz, matches Kairos */


extern void FreeRTOS_Tick_Handler( void );

/* The scheduler sweep (workload_freertos_schedsweep.c) records per-tick scheduler cost
 * when it is active; returns nonzero while sampling. */
extern int freertos_schedsweep_sample( uint32_t cost );


/* configSETUP_TICK_INTERRUPT(): called from xPortStartScheduler. */
void vConfigureTickInterrupt( void )
{
    GT_CTRL    = 0u;
    GT_ISR     = 1u;
    GT_AUTOINC = GT_RELOAD;
    GT_CMPL    = GT_CNTL + GT_RELOAD;
    GT_CMPH    = GT_CNTH;
    GT_CTRL    = ( 1u << 3 ) | ( 1u << 2 ) | ( 1u << 1 ) | ( 1u << 0 );  /* auto-inc | IRQ | comp | enable */

    /* Tick at the lowest *usable* priority: this GIC has 5 priority bits, so the port's
     * unmask value 255 reads back as 0xF8, and an interrupt signals only when its priority
     * is strictly below the mask. portLOWEST_INTERRUPT_PRIORITY (0xF8) would equal the mask
     * and stay blocked forever, so use portLOWEST_USABLE_INTERRUPT_PRIORITY (0xF0). */
    GICD_IPRIORITY[ TICK_INT_ID ] = ( uint8_t ) ( portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT );
    GICD_ISENABLER0 |= ( 1u << TICK_INT_ID );
}


/* configCLEAR_TICK_INTERRUPT(): called at the end of FreeRTOS_Tick_Handler. */
void vClearTickInterrupt( void )
{
    GT_ISR = 1u;
}


/* The CA9 port's FreeRTOS_IRQ_Handler reads ICCIAR, calls this, then writes ICCEOIR. */
void vApplicationIRQHandler( uint32_t ulICCIAR )
{
    uint32_t ulID = ulICCIAR & 0x3FFu;

    if( ulID == TICK_INT_ID )
    {
        /* Time the scheduler's per-tick work (xTaskIncrementTick: delayed-list servicing +
         * the switch decision), the N-dependent part comparable to the Kairos schedsweep. */
        uint32_t ulStart = pmu_cycles();
        FreeRTOS_Tick_Handler();
        freertos_schedsweep_sample( pmu_cycles() - ulStart );
    }
}
