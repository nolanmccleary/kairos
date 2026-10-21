#ifndef __IRQ_H__
#define __IRQ_H__

#include <stdint.h>
#include "cpu.h"
#include "min_heap.h"
#include "system.h"
#include "thread.h"



typedef enum
{
    GICD_IRQ = 0x1b,
}   hardware_irq_e;







sys_exit_e  mount_tick_irq(heap_t* heap, thread_t* ctx);
sys_exit_e  mount_tick_irq_to_core(heap_t* heap, thread_t* ctx);
sys_exit_e  unmount_tick_irq(heap_t* heap, thread_t* ctx);


#endif

