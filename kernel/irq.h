#ifndef __IRQ_H__
#define __IRQ_H__

#include <stdint.h>
#include "system.h"
#include "thread.h"



#define CYCLES_PER_TICK 800000 //@CLAUDE: please verify this
#define WCET_TICKS 1 //Each IRQ should run well within a tick

#define MAX_IRQS 100    //TODO: Right now this has to match MAX_THREADS


typedef enum
{
    GICD_IRQ = 0x1b,
}   hardware_irq_e;


typedef enum 
{
    IRQ_UNMOUNTED,
    IRQ_ARMED,
    IRQ_REMOVED,
}   irq_status_e;


typedef struct 
{
    size_t heap_index;
    size_t avail_index;
    irq_status_e status;
}   irq_metadata_t;


typedef struct
{
    uint32_t period;
    uint32_t release_time;
    uint32_t t0;
    uint32_t t1;
}   irq_timing_t;


typedef struct __attribute__((aligned(8))) //Stack should start 8-aligned
{
    irq_metadata_t md;
    irq_timing_t timing;

    sys_exit_e (*func)();

    uint32_t id;

    cpu_core_e core;
}   irq_t;





sys_exit_e  mount_tick_irq(irq_t* irq, sys_exit_e (*func)(void), uint32_t period, uint32_t id);
sys_exit_e  mount_tick_irq_to_core(irq_t* irq, cpu_core_e core, sys_exit_e (*func)(void), uint32_t period, uint32_t id);
sys_exit_e  unmount_tick_irq(irq_t* irq); //User still needs to de-allocate

void irq_switch_in(irq_t* irq);
void irq_switch_out(irq_t* irq);

#endif

