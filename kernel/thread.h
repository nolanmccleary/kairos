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

#define PERSISTENT_SLICE_TICKS 200


typedef enum
{
    CPU0 = 0,
    CPU1 = 1,
}   cpu_core_e;


typedef enum
{
    INITIALIZED,
    PENDING,
    RUNNING,
    FINISHED,
    REMOVED,
}   thread_status_e;


typedef enum
{
    APERIODIC,
    PERIODIC,
    PERSISTENT,
}   thread_periodicity_e;


typedef enum
{
    LOW_PRIORITY,
    MED_PRIORITY,
    HIGH_PRIORITY,
}   persistent_thread_priority_e;


typedef enum
{
    INCOMING_THREADS,
    DEAD_HEAP,
    REL_HEAP,
    FIFO_LOW,
    FIFO_MED,
    FIFO_HIGH,
}   thread_owner_e;


typedef struct
{
    thread_status_e status;
    thread_periodicity_e periodicity;
    persistent_thread_priority_e priority;
    thread_owner_e owner;
    size_t index;
}   thread_metadata_t;


typedef struct
{
    uint32_t period;
    uint32_t release_time;
    uint32_t deadline;
    uint32_t ticks_landed;
    bool dirty;
}   thread_timing_t;


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
}   thread_metrics_t;


//TODO: Pack this better
typedef struct __attribute__((aligned(8))) //Stack should start 8-aligned
{
    char stack[THREAD_STACK_SIZE];
    char* sp;

    thread_metadata_t md;
    thread_timing_t timing;
    thread_metrics_t metrics;

    sys_exit_e (*func)();

    uint32_t id;

    cpu_core_e core;
}   thread_t;


void init_metrics(thread_t* thread);

void switch_in(thread_t* thread);
void switch_out(thread_t* thread);


#endif

