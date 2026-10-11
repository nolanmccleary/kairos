#ifndef __THREAD_H__
#define __THREAD_H__


#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "system.h"


#define THREAD_STACK_SIZE 0x2000
#define MODE_SYS          0x1F   /* CPSR mode bits forged into a new thread's SPSR (see prime_thread) */
#define MAX_THREADS 100
#define IDLE_ID     0xFFu   /* main_thread's id -- marks idle ticks in the schedule trace */




typedef enum
{
    CPU0 = 0,
    CPU1 = 1,
}   cpu_core_e;



typedef enum
{
    PENDING,
    RUNNING,
    FINISHED,
}   thread_status_e;



typedef enum
{
    APERIODIC,
    PERIODIC,
}   thread_periodicity_e;


typedef enum 
{
    INCOMING_THREADS,
    OUTGOING_THREADS,
    DEAD_HEAP,
    REL_HEAP,
}   thread_owner_e;


typedef struct
{
    uint32_t ci;
    uint32_t ci_av;
    uint32_t prev_cycles;
    uint32_t delta_sum;

    uint32_t ti;
    uint32_t ti_av;
    uint32_t t0;

    uint32_t prev_u;
}   metrics_t;




//TODO: Pack this better
typedef struct __attribute__((aligned(8))) //Stack should start 8-aligned
{
    char stack[THREAD_STACK_SIZE];
    char* sp;

    thread_periodicity_e periodicity;
    uint32_t period;

    uint32_t release_time;
    uint32_t deadline;

    bool dirty;
    thread_status_e thread_status;

    sys_exit_e (*func)();

    metrics_t metrics;

    uint32_t id;
    size_t heap_index;
    thread_owner_e owner;

    cpu_core_e core;
}   thread_t;



void init_metrics(thread_t* thread);

void switch_in(thread_t* thread);
void switch_out(thread_t* thread);


#endif
