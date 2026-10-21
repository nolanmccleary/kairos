#ifndef __CPU_H__
#define __CPU_H__

#include <stdint.h>
#include <stdbool.h>
// #include "irq.h"
#include "lock.h"
#include "min_heap.h"
#include "thread.h"
#include "thread_fifo.h"

/* A critical section that fences the scheduler tick masks IRQ ("cpsid i") when the tick is
 * an IRQ. Under USE_FIQ the tick is an FIQ, which "cpsid i" does NOT mask, so those sections
 * must mask F as well. PREEMPT_MASK carries the right cpsid/cpsie operand for either build:
 * use it as `"cpsid " PREEMPT_MASK` / `"cpsie " PREEMPT_MASK` in inline asm. */
#if USE_FIQ
#define PREEMPT_MASK "if"
#else
#define PREEMPT_MASK "i"
#endif


#if ENABLE_SMP == 1
#define NUM_CPUS 2
#else
#define NUM_CPUS 1
#endif


typedef enum
{
    CPU_PSCHED_INIT_REQUEST,
    CPU_PSCHED_INIT_ACK,
    CPU_PSCHED_DEINIT_REQUEST,
    CPU_PSCHED_DEINIT_ACK,
    CPU_IRQ_PING,
    CPU_MUTEX_HOLD,
}   cpu_sgi_e;



typedef struct
{
    _Alignas(32) volatile bool sched_init;

    //Sched aggregate
    uint32_t ticks;
    uint32_t missed_deadlines;
    uint32_t utilization;
    uint32_t avg_scheduler_overhead;

    //Sched specific
    heap_t* irqHeap;

    heap_t* deadHeap;
    heap_t* relHeap;
    
    //TODO: Maybe array-ify this
    thread_fifo_t* lowFifo;
    thread_fifo_t* medFifo;
    thread_fifo_t* highFifo;

    thread_t* curr_thread;
    thread_t* main_thread;
    
    mutex_t thread_mutex; //TODO: Rename to core mutex. we can then add thread and irq subsection mutexes later
    mutex_t irq_mutex;

    bool request_terminate;

    thread_fifo_t* incoming_threads;
    thread_fifo_t* outgoing_threads;

}   cpu_t;


extern volatile uint32_t g_cpu_mailbox_uncached;
extern cpu_t g_cpus[NUM_CPUS];



cpu_core_e least_loaded_core(void);

cpu_core_e curr_core(void);
void update_cpu_scheduler_overhead(cpu_core_e cpu, uint32_t overhead);

void update_utilization(thread_t* thread);

void send_cpu_interrupt(cpu_sgi_e interrupt);

#endif

