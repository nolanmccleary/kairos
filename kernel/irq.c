#include "aux.h"
#include "cpu.h"
#include "pmu.h"
#include "irq.h"
#include "lock.h"
#include "bsp.h"
#include "cpu.h"
#include "timers.h"
#include "sched.h"
#include "ktrace.h"
#include "min_heap.h"


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
            cpu_core_e core = curr_core();
            if (lock_mutex_best_effort(&(g_cpus[core].core_mutex)) == LOCK_OK)
            {
                GTIMER_ISR = 1; //Timer ISR ACK, when this
                next_thread(core);
                unlock_mutex(&(g_cpus[core].core_mutex));
            }
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
    c_irq_handler(id);  //Right now FIQ is configured to route through IRQ handler if enabled
#else
    (void)id;
#endif
}



sys_exit_e  mount_tick_irq(irq_t* irq, sys_exit_e (*func)(void), uint32_t period, uint32_t id)
{
    cpu_core_e core = least_loaded_core();
    mount_tick_irq_to_core(irq, core, func, period, id);
    return SYS_OK;
}


sys_exit_e  mount_tick_irq_to_core(irq_t* irq, cpu_core_e core, sys_exit_e (*func)(void), uint32_t period, uint32_t id)
{
    irq->core = core;
    irq->func = func;
    irq->timing.period = period;
    irq->id = id;

    lock_mutex_persistent(&g_cpus[core].core_mutex);
    
    if (g_cpus[core].irqHeap->curr_index == MAX_THREADS) //Heap full = no free spots
    {
        unlock_mutex(&g_cpus[core].core_mutex);
        return SYS_ERROR;
    }

    for (size_t i = 0; i < MAX_IRQS; i++) //Should always find something
    {
        if (g_cpus[core].avail[i] == NULL) //Found free spot
        {
            if (i == 0) //First position is free 
            {
                if (g_cpus[core].irqHeap->curr_index > 0) //There must be other nonfree entries ahead of this one
                {
                    size_t j = 1;
                    while (j < MAX_IRQS)
                    {
                        if (g_cpus[core].avail[j] != NULL) break;
                        j++;
                    }
                    
                    irq_t* next = g_cpus[core].avail[j];
                    uint32_t rel = next->timing.release_time - (j * WCET_TICKS);

                    while (lt_wrapped(rel, g_cpus[core].ticks)) rel += next->timing.period;

                    irq->timing.release_time = rel;
                }

                else //no tick irqs mounted yet 
                {
                    irq->timing.release_time = g_cpus[core].ticks; //Will fire next sched preemption
                }
            }

            else //space directly behind was taken 
            {
                irq->timing.release_time = g_cpus[core].avail[i-1]->timing.release_time + WCET_TICKS;
            }

            g_cpus[core].avail[i] = irq;
            irq->md.avail_index = i;

            break;
        }
    }
    
    min_heap_insert_node(g_cpus[core].irqHeap, (void*)irq, irq->timing.release_time); 
    irq->md.status = IRQ_ARMED;

    unlock_mutex(&g_cpus[core].core_mutex);
    return SYS_OK;
}



sys_exit_e unmount_tick_irq(irq_t* irq)
{
    cpu_core_e core = irq->core;

    lock_mutex_persistent(&g_cpus[core].core_mutex);

    min_heap_remove_node(g_cpus[core].irqHeap, irq->md.heap_index);
    g_cpus[core].avail[irq->md.avail_index] = NULL;
    irq->md.status = IRQ_REMOVED;

    unlock_mutex(&g_cpus[irq->core].core_mutex);
    return SYS_OK;
}



void irq_switch_in(irq_t* irq)
{
    irq->timing.t0 = pmu_cycles(); 
}



void irq_switch_out(irq_t* irq)
{
    irq->timing.t1 = pmu_cycles(); 
}
