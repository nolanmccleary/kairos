/* Kairos scheduler: a hybrid Earliest-Deadline-First + round-robin policy, driven once
 * per tick from the IRQ handler (next_thread). EDF tasks run from twin min-heaps (one by
 * deadline, one by release time); round-robin persistent tasks fill the slack. */
#include <stddef.h>
#include <stdint.h>
#include "aux.h"
#include "cpu.h"
#include "lock.h"
#include "pmu.h"
#include "sched.h"
#include "tlsf.h"
#include "system.h"
#include "thread.h"
#include "min_heap.h"
#include "thread_fifo.h"
#include "ktrace.h"
#include "fault.h"
#include "telemetry.h"


static heap_t heap1[NUM_CPUS];
static heap_t heap2[NUM_CPUS];
static thread_fifo_t incoming_fifos[NUM_CPUS];
static thread_fifo_t outgoing_fifos[NUM_CPUS];
static thread_fifo_t low_fifos[NUM_CPUS];
static thread_fifo_t med_fifos[NUM_CPUS];
static thread_fifo_t high_fifos[NUM_CPUS];


/* The tick handler runs the scheduler in IRQ mode in BOTH builds: the IRQ path enters it
 * directly, and the FIQ path (kernel.s _fiq_handler) drops from FIQ to IRQ mode before the
 * C runs, specifically so r8-r12 are the common bank, not the FIQ bank. So these SP macros
 * always return to IRQ mode (0x12) after touching the SYS sp. */
#define SCHED_HANDLER_MODE "0x12"

#define SAVE_SYS_SP(dst)                           \
    __asm__ __volatile__(                          \
        "cps #0x1f\n\t"                            \
        "mov r0, sp\n\t"                           \
        "cps #" SCHED_HANDLER_MODE "\n\t"           \
        "mov %0, r0\n\t"                           \
        : "=r"(dst)                                \
        :                                          \
        : "r0", "memory")

#define LOAD_SYS_SP(src)                           \
    __asm__ __volatile__(                          \
        "mov r0, %0\n\t"                           \
        "cps #0x1f\n\t"                            \
        "mov sp, r0\n\t"                           \
        "cps #" SCHED_HANDLER_MODE "\n\t"           \
        :                                          \
        : "r"(src)                                 \
        : "r0", "memory")

#define SWITCH_FRAME_AND_RETURN(sp) \
    __asm__ __volatile__("mov sp, %0\n\t" "pop {lr}\n\t" "pop {r0-r12}\n\t" "RFEFD sp!\n\t" :: "r"(sp) : "memory")


//////////////////////////////// STATICS //////////////////////////////////////////


static void thread_exit();

//assumes thread mutex is already held here
static inline void prime_thread(thread_t* thread)
{
    uint32_t* sp = (uint32_t*)(((uintptr_t)(thread->stack + THREAD_STACK_SIZE)) & ~(uintptr_t)0x7);

    //Return From Exception Full Descending (RFEFD) frame (bottom) as well as all the popping stuff etc
    *(--sp) = MODE_SYS;                    // spsr_irq
    *(--sp) = (uint32_t)thread->func;      // lr_irq → pc

    // r0-r12 (full integer file; the IRQ handler saves/restores all of them)
    *(--sp) = 0; // r12
    *(--sp) = 0; // r11
    *(--sp) = 0; // r10
    *(--sp) = 0; // r9
    *(--sp) = 0; // r8
    *(--sp) = 0; // r7
    *(--sp) = 0; // r6
    *(--sp) = 0; // r5
    *(--sp) = 0; // r4
    *(--sp) = 0; // r3
    *(--sp) = 0; // r2
    *(--sp) = 0; // r1
    *(--sp) = 0; //(uint32_t)&thread->md.status; // r0

    *(--sp) = (uint32_t)thread_exit; //lr

    thread->sp = (char*)sp;
    thread->md.status = RUNNING;
}


//Assumes thread lock held before invocation
static inline bool next_up(cpu_core_e core)
{
    thread_t* thread;

    while(g_cpus[core].incoming_threads->size > 0)
    {
        thread = fifo_pop(g_cpus[core].incoming_threads);

        thread->timing.deadline = g_cpus[core].ticks + thread->timing.period;
        thread->timing.release_time = g_cpus[core].ticks;
        thread->timing.dirty = false;
        thread->md.status = PENDING;

        if (thread->md.periodicity == PERSISTENT)
        {
            if (thread->md.priority == LOW_PRIORITY)
            {
                thread->md.owner = FIFO_LOW;
                fifo_push(g_cpus[core].lowFifo, thread);
            }

            else if (thread->md.priority == MED_PRIORITY)
            {
                thread->md.owner = FIFO_MED;
                fifo_push(g_cpus[core].medFifo, thread);
            }

            else if (thread->md.priority == HIGH_PRIORITY)
            {
                thread->md.owner = FIFO_HIGH;
                fifo_push(g_cpus[core].highFifo, thread);
            }

            else raise_error("NEXT_UP: Invalid assigned priority for persistent task");

            continue;   // persistent threads live in the priority FIFOs, not the EDF heaps
        }

        else
        {
            thread->md.owner = DEAD_HEAP;
            insert_node(g_cpus[core].deadHeap, thread, thread->timing.deadline);
        }
    }

    bool thread_found = false;

    while (g_cpus[core].relHeap->curr_index > 0 && geq_wrapped(g_cpus[core].ticks, g_cpus[core].relHeap->heap[0].thread->timing.release_time))
    {
        pop_heap(g_cpus[core].relHeap, &thread);

        thread->md.owner = DEAD_HEAP;
        insert_node(g_cpus[core].deadHeap, thread, thread->timing.deadline);
    }

    while(g_cpus[core].deadHeap->curr_index > 0)
    {
        thread = g_cpus[core].deadHeap->heap[0].thread;

        if (g_cpus[core].curr_thread != g_cpus[core].main_thread)
        {
            char* asp  = g_cpus[core].curr_thread->sp;
            char* base = g_cpus[core].curr_thread->stack;
            if (asp < base || asp > base + THREAD_STACK_SIZE) raise_error("next_up: active thread sp outside stack range");
        }

        if (thread->md.status == FINISHED)
        {
            if (thread->md.periodicity == PERIODIC)
            {
                if (thread->timing.period == 0) raise_error("next_up: thread period corrupted");

                do
                {
                    thread->timing.deadline += thread->timing.period;
                }   while (thread->timing.deadline <= g_cpus[core].ticks);

                do
                {
                    thread->timing.release_time += thread->timing.period;
                }   while (thread->timing.release_time < thread->timing.deadline - thread->timing.period);

                thread->timing.dirty = false;
                thread->md.status = PENDING;

                if (lt_wrapped(g_cpus[core].ticks, thread->timing.release_time))
                {
                    remove_head(g_cpus[core].deadHeap);
                    thread->md.owner = REL_HEAP;
                    insert_node(g_cpus[core].relHeap, thread, thread->timing.release_time);
                }

                else balance_node(g_cpus[core].deadHeap, thread->md.heap_index, thread->timing.deadline);
                continue;
            }

            else if (thread->md.periodicity == APERIODIC)
            {
                remove_head(g_cpus[core].deadHeap);
                thread->md.owner = OUTGOING_THREADS;
                fifo_push(g_cpus[core].outgoing_threads, thread);
                continue;
            }

            else
            {
                raise_error("next_up: thread periodicity corrupted");
            }
        }

        if(thread->md.status == PENDING || thread->md.status == RUNNING)
        {
            if (geq_wrapped(g_cpus[core].ticks, thread->timing.deadline) && !thread->timing.dirty)
            {
                thread->timing.dirty = true;
                g_cpus[core].missed_deadlines++;
            }

            g_cpus[core].curr_thread = thread;
            thread_found = true;
            break;
        }

        else if (thread->md.status != FINISHED) raise_error("next_up: thread status corrupted");
    }


    thread_fifo_t* fifo;


    //Yes I know this is repeated code
    if (!thread_found)
    {
        fifo = g_cpus[core].highFifo;

        while (fifo->size > 0)
        {
            thread = fifo->fifo[fifo->start];

            if (thread->md.status != FINISHED)
            {
                thread->timing.ticks_landed++;

                if (geq_wrapped(g_cpus[core].ticks, thread->timing.deadline))
                {
                    thread->timing.release_time = g_cpus[core].ticks;
                    thread->timing.deadline = g_cpus[core].ticks + thread->timing.period;

                    //Will switch when <period> ticks spent inside thread; Persistent thread may be preempted by periodic halfway through for example
                    //When available to run again we should go back to the last persistent thread that was filling its window.
                    if (fifo->size > 1 && thread->timing.ticks_landed >= thread->timing.period)
                    {
                        thread->timing.ticks_landed = 0;
                        fifo_push(fifo, fifo_pop(fifo));
                        continue;
                    }
                }


                g_cpus[core].curr_thread = thread;
                thread_found = true;
                break;
            }

            else fifo_push(g_cpus[core].outgoing_threads, fifo_pop(fifo));
        }
    }


   if (!thread_found)
    {
        fifo = g_cpus[core].medFifo;

        while (fifo->size > 0)
        {
            thread = fifo->fifo[fifo->start];

            if (thread->md.status != FINISHED)
            {
                thread->timing.ticks_landed++;

                if (geq_wrapped(g_cpus[core].ticks, thread->timing.deadline))
                {
                    thread->timing.release_time = g_cpus[core].ticks;
                    thread->timing.deadline = g_cpus[core].ticks + thread->timing.period;

                    if (fifo->size > 1 && thread->timing.ticks_landed >= thread->timing.period)
                    {
                        thread->timing.ticks_landed = 0;
                        fifo_push(fifo, fifo_pop(fifo));
                        continue;
                    }
                }


                g_cpus[core].curr_thread = thread;
                thread_found = true;
                break;
            }

            else fifo_push(g_cpus[core].outgoing_threads, fifo_pop(fifo));
        }
    }


    if (!thread_found)
    {
        fifo = g_cpus[core].lowFifo;

        while (fifo->size > 0)
        {
            thread = fifo->fifo[fifo->start];

            if (thread->md.status != FINISHED)
            {
                thread->timing.ticks_landed++;

                if (geq_wrapped(g_cpus[core].ticks, thread->timing.deadline))
                {
                    thread->timing.release_time = g_cpus[core].ticks;
                    thread->timing.deadline = g_cpus[core].ticks + thread->timing.period;

                    if (fifo->size > 1 && thread->timing.ticks_landed >= thread->timing.period)
                    {
                        thread->timing.ticks_landed = 0;
                        fifo_push(fifo, fifo_pop(fifo));
                        continue;
                    }
                }


                g_cpus[core].curr_thread = thread;
                thread_found = true;
                break;
            }

            else fifo_push(g_cpus[core].outgoing_threads, fifo_pop(fifo));
        }
    }


    if ((g_cpus[core].outgoing_threads->size > 0) && (lock_mutex_best_effort(&g_allocator_mutex) == LOCK_OK))
    {
        while (g_cpus[core].outgoing_threads->size > 0)
        {
            thread = fifo_pop(g_cpus[core].outgoing_threads);
            g_cpus[core].utilization -= thread->metrics.prev_u;
            kFree(thread);
        }

        unlock_mutex(&g_allocator_mutex);
    }

    return thread_found;
}


static void thread_exit()
{
#if USE_FIQ
    __asm__ __volatile__("cpsid if" ::: "memory");   /* the FIQ tick must not preempt this critical section */
#else
    __asm__ __volatile__("cpsid i" ::: "memory");
#endif

    cpu_core_e core = curr_core();
    lock_mutex_persistent(&(g_cpus[core].thread_mutex));


    g_cpus[core].curr_thread->md.status = FINISHED;
    switch_out(g_cpus[core].curr_thread);

    if (!next_up(core)) //Either no valid tasks set or we ran the last one last cycle
    {
        g_cpus[core].curr_thread = g_cpus[core].main_thread;
    }

    switch_in(g_cpus[core].curr_thread);

    switch (g_cpus[core].curr_thread->md.status)
    {
        case PENDING:
            prime_thread(g_cpus[core].curr_thread);
            /* fall through */

        case RUNNING:
        {
            char* new_sp = g_cpus[core].curr_thread->sp;

            unlock_mutex(&(g_cpus[core].thread_mutex));

            SWITCH_FRAME_AND_RETURN(new_sp);
            __builtin_unreachable();
        }

            break;

        case FINISHED:
            break;

        default:
            break;
    }

    //Shouldn't get hit
    for (;;) {};
}


/////////////////////////////////////////////// END OF STATICS ///////////////////////////////


sys_exit_e psched_core_init(void)
{
    cpu_core_e core = curr_core();
    lock_mutex_persistent(&(g_cpus[core].thread_mutex));   // serializes the rebuild vs next_thread; no cpsid needed

    char* sp;
    __asm__ __volatile__ ("mov %0, sp" : "=r"(sp));
    g_cpus[core].ticks = 0;
    g_cpus[core].missed_deadlines = 0;

    g_cpus[core].deadHeap = &heap1[core];
    g_cpus[core].deadHeap->curr_index = 0;

    g_cpus[core].relHeap = &heap2[core];
    g_cpus[core].relHeap->curr_index = 0;

    g_cpus[core].utilization = 0;


    g_cpus[core].incoming_threads = &incoming_fifos[core];
    g_cpus[core].outgoing_threads = &outgoing_fifos[core];
    g_cpus[core].lowFifo = &low_fifos[core];
    g_cpus[core].medFifo = &med_fifos[core];
    g_cpus[core].highFifo = &high_fifos[core];

    initialize_fifo(g_cpus[core].incoming_threads);
    initialize_fifo(g_cpus[core].outgoing_threads);
    initialize_fifo(g_cpus[core].lowFifo);
    initialize_fifo(g_cpus[core].medFifo);
    initialize_fifo(g_cpus[core].highFifo);

    lock_mutex_persistent(&g_allocator_mutex);
    g_cpus[core].main_thread = (thread_t*)kMalloc(sizeof(thread_t));
    unlock_mutex(&g_allocator_mutex);

    if (g_cpus[core].main_thread == NULL) //can't bring up the core without an idle/boot TCB
    {
        unlock_mutex(&(g_cpus[core].thread_mutex));
        return SYS_ERROR;
    }

    g_cpus[core].main_thread->md.status = RUNNING;
    g_cpus[core].main_thread->sp = sp;
    g_cpus[core].main_thread->core = core;
    g_cpus[core].main_thread->md.periodicity = APERIODIC;
    g_cpus[core].main_thread->id = IDLE_ID;
    init_metrics(g_cpus[core].main_thread);
    g_cpus[core].curr_thread = g_cpus[core].main_thread;
    g_cpus[core].sched_init = true;
    g_cpus[core].request_terminate = false;

    unlock_mutex(&(g_cpus[core].thread_mutex));

    __asm__ __volatile__("dmb sy" ::: "memory");

    return SYS_OK;
}


sys_exit_e psched_core_deinit(void)
{
    cpu_core_e core = curr_core();

    lock_mutex_persistent(&(g_cpus[core].thread_mutex));
    g_cpus[core].request_terminate = true;
    unlock_mutex(&(g_cpus[core].thread_mutex));

    __asm__ __volatile__("dmb sy" ::: "memory");

    return SYS_OK;
}


sys_exit_e psched_init(void)
{
    psched_core_init();

#if ENABLE_SMP
    uint32_t prev = g_spin_exit;
    g_spin_exit = 0;

    send_cpu_interrupt(CPU_PSCHED_INIT_REQUEST);

    while (!g_spin_exit)
    {
        __asm__ volatile ("wfi");
    };

    g_spin_exit = prev;
#endif
    return SYS_OK;
}


sys_exit_e psched_deinit(void)
{
    psched_core_deinit();

#if ENABLE_SMP
    uint32_t prev = g_spin_exit;
    g_spin_exit = 0;

    send_cpu_interrupt(CPU_PSCHED_DEINIT_REQUEST);

    while (!g_spin_exit)
    {
        __asm__ volatile ("wfi");
    };

    g_spin_exit = prev;
#endif
    return SYS_OK;
}


static inline thread_t* enqueue_new_thread(cpu_core_e core, sys_exit_e (*func)(void), uint32_t period, thread_periodicity_e periodicity, uint32_t id)
{
    if (period == 0u) return NULL; //a zero period collapses deadline onto release_time; disallow it at admission

    lock_mutex_persistent(&g_allocator_mutex);
    thread_t* new_thread = (thread_t*)kMalloc(sizeof(thread_t));
    unlock_mutex(&g_allocator_mutex);

    if (new_thread == NULL) return NULL;   //heap exhausted

    new_thread->timing.period = period;
    new_thread->md.periodicity = periodicity;
    new_thread->id = id;
    new_thread->md.owner = INCOMING_THREADS;

    new_thread->func = func;
    new_thread->sp = (char*)(((uintptr_t)(new_thread->stack + THREAD_STACK_SIZE)) & ~(uintptr_t)0x7); //8-byte align sp so processor doesn't abort

    new_thread->core = core;
    init_metrics(new_thread);

    lock_mutex_persistent(&(g_cpus[core].thread_mutex));
    fifo_push(g_cpus[core].incoming_threads, new_thread);
    unlock_mutex(&(g_cpus[core].thread_mutex));

    return new_thread;
}


static inline cpu_core_e least_loaded_core(void)
{
    static cpu_core_e tie_default = CPU0;

    if (g_cpus[CPU0].utilization < g_cpus[CPU1].utilization) return CPU0;
    if (g_cpus[CPU1].utilization < g_cpus[CPU0].utilization) return CPU1;

    cpu_core_e core = tie_default;
    tie_default = (tie_default == CPU0) ? CPU1 : CPU0;
    return core;
}


thread_t* add_periodic_thread(sys_exit_e (*func)(void), uint32_t period, thread_periodicity_e periodicity, uint32_t id)
{
    return enqueue_new_thread(least_loaded_core(), func, period, periodicity, id);
}


thread_t* add_periodic_thread_to_core(cpu_core_e core, sys_exit_e (*func)(void), uint32_t period, thread_periodicity_e periodicity, uint32_t id)
{
    return enqueue_new_thread(core, func, period, periodicity, id);
}


static inline thread_t* enqueue_new_persistent_thread(cpu_core_e core, sys_exit_e (*func)(void), uint32_t id, persistent_thread_priority_e priority)
{
    lock_mutex_persistent(&g_allocator_mutex);
    thread_t* new_thread = (thread_t*)kMalloc(sizeof(thread_t));
    unlock_mutex(&g_allocator_mutex);

    if (new_thread == NULL) return NULL;

    new_thread->timing.period = PERSISTENT_SLICE_TICKS;                 // unused for persistent; sliced by PERSISTENT_SLICE_TICKS
    new_thread->timing.ticks_landed = 0;
    new_thread->md.periodicity = PERSISTENT;
    new_thread->md.priority = priority;
    new_thread->id = id;
    new_thread->md.owner = INCOMING_THREADS;

    new_thread->func = func;
    new_thread->sp = (char*)(((uintptr_t)(new_thread->stack + THREAD_STACK_SIZE)) & ~(uintptr_t)0x7); //8-byte align sp so processor doesn't abort

    new_thread->core = core;
    init_metrics(new_thread);

    lock_mutex_persistent(&(g_cpus[core].thread_mutex));
    fifo_push(g_cpus[core].incoming_threads, new_thread);
    unlock_mutex(&(g_cpus[core].thread_mutex));

    return new_thread;
}


thread_t* add_persistent_thread(sys_exit_e (*func)(void), uint32_t id, persistent_thread_priority_e priority)
{
    return enqueue_new_persistent_thread(least_loaded_core(), func, id, priority);
}


thread_t* add_persistent_thread_to_core(cpu_core_e core, sys_exit_e (*func)(void), uint32_t id, persistent_thread_priority_e priority)
{
    return enqueue_new_persistent_thread(core, func, id, priority);
}


sys_exit_e kill_thread(thread_t* thread)
{
    lock_mutex_persistent(&(g_cpus[thread->core].thread_mutex));

    thread->md.periodicity = APERIODIC;
    thread->md.status = FINISHED;
    thread->md.owner = OUTGOING_THREADS;
    g_cpus[thread->core].utilization -= thread->metrics.prev_u;
    thread->metrics.prev_u = 0;
    unlock_mutex(&(g_cpus[thread->core].thread_mutex));

    return SYS_OK;
}


sys_exit_e suspend_thread(thread_t* thread, uint32_t delay)
{
    cpu_core_e core = thread->core;
    cpu_core_e currCore = curr_core();

    lock_mutex_persistent(&(g_cpus[core].thread_mutex));

    sys_exit_e ret = SYS_OK;

    if ((thread->md.status == FINISHED && thread->md.periodicity == APERIODIC) || thread->md.owner == OUTGOING_THREADS || thread->md.owner == INCOMING_THREADS) ret = SYS_ERROR; //Can't suspend something that's finished and won't run again

    if (ret == SYS_OK)
    {
        uint32_t ticks = g_cpus[core].ticks;
        if (geq_wrapped(thread->timing.release_time, ticks + delay)) ret = SYS_ERROR; //Can't run until at least delay ticks anyway

        if (ret == SYS_OK)
        {
            thread->timing.release_time = ticks + delay;

            if (geq_wrapped(ticks, thread->timing.deadline)) //If past deadline start clean
            {
                thread->timing.deadline = thread->timing.release_time + thread->timing.period;
            }

            else
            {
                uint32_t delta = thread->timing.deadline - ticks;
                thread->timing.deadline = thread->timing.release_time + delta;
            }

            if (thread->md.owner == DEAD_HEAP)
            {
                thread->md.owner = REL_HEAP;
                remove_node(g_cpus[core].deadHeap, thread->md.heap_index);
                insert_node(g_cpus[core].relHeap, thread, thread->timing.release_time);
            }

            else if(thread->md.owner == REL_HEAP)
            {
                balance_node(g_cpus[core].relHeap, thread->md.heap_index, thread->timing.release_time);
            }
        }
    }


    if (ret == SYS_OK && currCore == core && thread == g_cpus[core].curr_thread) //If the target thread is the current one we need to force a context switch
    {
        void* resume_pc = &&resume;
        void* ra = __builtin_return_address(0);
        uint32_t curr_cpsr;

        __asm__ goto (
            "mrs %0, cpsr\n"
            "push {%0}\n"
            "push {%2}\n"
            "push {r0-r12}\n"
            "push {%3}\n"

            "mov %1, sp\n"

            :   "=&r"(curr_cpsr),"=r"(g_cpus[core].curr_thread->sp)
            :   "r"(resume_pc),"r"(ra)
            :   "memory"
            :   resume
        );

        switch_out(g_cpus[core].curr_thread);
        if (!next_up(core))
        {
            g_cpus[core].curr_thread = g_cpus[core].main_thread;
        }
        switch_in(g_cpus[core].curr_thread);

        char* new_sp = g_cpus[core].curr_thread->sp;

        /* The scheduler is now mid-switch: curr_thread points at the next thread while sp_sys
         * is still this one's. The mutex guarded that up to here, but it has to be released
         * before we leave (the next thread needs it) and we never return to unlock later. So
         * fence the tick across the unlock -> frame-switch window; the RFEFD inside
         * SWITCH_FRAME_AND_RETURN restores the next thread's cpsr, re-enabling it. This is the
         * same protection thread_exit gets from its leading cpsid. */
        __asm__ __volatile__("cpsid " PREEMPT_MASK ::: "memory");
        unlock_mutex(&(g_cpus[core].thread_mutex));

        SWITCH_FRAME_AND_RETURN(new_sp);
        __builtin_unreachable();
    }


    else unlock_mutex(&(g_cpus[core].thread_mutex));


    resume:
        return ret;
}


inline void next_thread()
{
    uint32_t overhead = pmu_cycles();
    cpu_core_e core = curr_core();

#ifdef MODE_TEST
    thread_t* sched_prev = g_cpus[core].curr_thread;
#endif

    if (lock_mutex_best_effort(&(g_cpus[core].thread_mutex)) == LOCK_OK)
    {
        if (g_cpus[core].sched_init && !g_cpus[core].request_terminate)
        {
            switch_out(g_cpus[core].curr_thread);
            update_utilization(g_cpus[core].curr_thread);

            g_cpus[core].ticks++;

            SAVE_SYS_SP(g_cpus[core].curr_thread->sp);


            if (g_cpus[core].curr_thread != g_cpus[core].main_thread)
            {
                char* asp  = g_cpus[core].curr_thread->sp;
                char* base = g_cpus[core].curr_thread->stack;
                if (asp < base || asp > base + THREAD_STACK_SIZE) raise_error("next_thread: active thread sp outside stack range");
            }


#ifdef MODE_TEST
            /*
            * Python finished sampling the current EDF trial.
            *
            * At overload, EDF may otherwise starve main forever.
            * Force main in once so KTRACE_WAIT_RELEASE() can observe
            * g_test_release and return.
            */
            if (KTRACE_RELEASE_PENDING())
            {
                g_cpus[core].curr_thread = g_cpus[core].main_thread;

                LOAD_SYS_SP(g_cpus[core].main_thread->sp);

                unlock_mutex(&(g_cpus[core].thread_mutex));
                return;
            }
#endif


            if (!next_up(core)) //Either no valid tasks set or we ran the last one last cycle
            {
                g_cpus[core].curr_thread = g_cpus[core].main_thread;
            }

            switch_in(g_cpus[core].curr_thread);
            KTRACE_TICK_EXIT(g_cpus[core].curr_thread);   /* test-only per-tick hook (Gantt + metrics mirror) */

            switch (g_cpus[core].curr_thread->md.status)
            {
                case PENDING:
                    prime_thread(g_cpus[core].curr_thread);
                    /* fall through */

                case RUNNING:
                    LOAD_SYS_SP(g_cpus[core].curr_thread->sp);
                    break;

                default:
                    break;
            }
        }


        else if (g_cpus[core].request_terminate && (lock_mutex_best_effort(&g_allocator_mutex) == LOCK_OK))
        {
            destroy_threads(g_cpus[core].incoming_threads);
            destroy_threads(g_cpus[core].outgoing_threads);

            for (size_t i = 0; i < g_cpus[core].deadHeap->curr_index; i++)
            {
                if (g_cpus[core].deadHeap->heap[i].thread != NULL)
                {
                    kFree(g_cpus[core].deadHeap->heap[i].thread);
                }
            }
            g_cpus[core].deadHeap->curr_index = 0;

            for (size_t i = 0; i < g_cpus[core].relHeap->curr_index; i++)
            {
                if (g_cpus[core].relHeap->heap[i].thread != NULL)
                {
                    kFree(g_cpus[core].relHeap->heap[i].thread);
                }
            }
            g_cpus[core].relHeap->curr_index = 0;

            if (g_cpus[core].curr_thread != g_cpus[core].main_thread)
            {
                LOAD_SYS_SP(g_cpus[core].main_thread->sp);
            }
            __asm__ __volatile__ ("dmb sy" ::: "memory");
            kFree(g_cpus[core].main_thread);

            g_cpus[core].sched_init = false;
            g_cpus[core].request_terminate = false;

            unlock_mutex(&g_allocator_mutex);
        }

        unlock_mutex(&(g_cpus[core].thread_mutex));
    }

    overhead = pmu_cycles() - overhead;
    update_cpu_scheduler_overhead(core, overhead);

    KTRACE_SCHED_COST(overhead, g_cpus[core].curr_thread != sched_prev);
}

