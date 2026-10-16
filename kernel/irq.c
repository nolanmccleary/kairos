#include "irq.h"
#include "lock.h"
#include "bsp.h"
#include "cpu.h"
#include "timers.h"
#include "sched.h"
#include "ktrace.h"


void c_irq_handler(int id)
{
    /* GICC_IAR carries the source CPUID in bits [12:10] for SGIs; strip it so a
     * cross-core SGI (ACK from the other core) matches its cpu_sgi_e case. */
    switch(id & 0x3FF)
    {

        case CPU_PSCHED_INIT_REQUEST:
            psched_core_init();
            send_cpu_interrupt(CPU_PSCHED_INIT_ACK);
            break;


        case CPU_PSCHED_INIT_ACK:
            g_spin_exit = 1;
            break;


        case CPU_PSCHED_DEINIT_REQUEST:
            psched_core_deinit();
            send_cpu_interrupt(CPU_PSCHED_DEINIT_ACK);
            break;


        case CPU_PSCHED_DEINIT_ACK:
            g_spin_exit = 1;
            break;


        case GICD_IRQ:
            // WDT_L4 = 0x76; //Feed WDT
            GTIMER_ISR = 1; //Timer ISR ACK, when this
            next_thread();
            break;


#ifdef MODE_TEST
        case CPU_IRQ_PING:
            KTRACE_IRQ_PING();
            break;


        case CPU_MUTEX_HOLD:
            KTRACE_MUTEX_HOLD();
            break;
#endif


        default:
            break;
    }
}


void c_fiq_handler(int id)
{
#if USE_FIQ
    /* USE_FIQ: every GIC interrupt (the tick and the SMP SGIs) arrives as an FIQ because
     * GICC_CTLR.FIQEn is set and all interrupts stay in Group 0. Dispatch is identical to
     * the IRQ build, so just hand it to c_irq_handler. The full-context FIQ entry in
     * kernel.s already saved the interrupted thread, so a tick can switch. */
    c_irq_handler(id);
#else
    (void)id;
#endif
}

