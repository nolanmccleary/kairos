#include "gic.h"

/* GICD_IRQ (0x1b = 27) is the global-timer PPI that drives the scheduler tick. */
#define TICK_ID 27u

/* USE_FIQ routes GIC interrupts to the FIQ signal instead of IRQ by setting GICC_CTLR.FIQEn
 * (bit 3). Everything stays in Group 0, so which interrupts are enabled and how they are
 * acknowledged (GICC_IAR / GICC_EOIR in _identify_and_clear_source) is byte-for-byte the
 * same as the IRQ build -- only the CPU exception taken changes from IRQ to FIQ. The FIQ
 * handler then services the tick and the SMP SGIs exactly as c_irq_handler did. */
#if USE_FIQ
#define GICC_CTLR_VAL  ( ( 1u << 0 ) | ( 1u << 3 ) )   /* EnableGrp0 | FIQEn */
#else
#define GICC_CTLR_VAL  1u
#endif


void cpu0_gic_init(void)
{
    GICD_CTLR       = 1;
    GICD_ISENABLER0 |= (1 << TICK_ID);
    GICC_PMR        = 0xFF;
    GICC_CTLR       = GICC_CTLR_VAL;
}


void cpu1_gic_init(void)
{
    GICD_ISENABLER0 |= (1 << TICK_ID);
    GICC_PMR        = 0xFF;
    GICC_CTLR       = GICC_CTLR_VAL;
}
