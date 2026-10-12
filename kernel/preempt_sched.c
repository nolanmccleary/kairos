#include <stddef.h>
#include <stdint.h>
#include "aux.h"
#include "cpu.h"
#include "lock.h"
#include "pmu.h"
#include "preempt_sched.h"
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



#define SAVE_SYS_SP(dst) \
    __asm__ __volatile__("cps #0x1f\n\t" "mov %0, sp\n\t" "cps #0x12\n\t" : "=r"(dst))

#define LOAD_SYS_SP(src) \
    __asm__ __volatile__("cps #0x1f\n\t" "mov sp, %0\n\t" "cps #0x12\n\t" :: "r"(src) : "memory")

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
    *(--sp) = 0; //(uint32_t)&thread->thread_status; // r0

    *(--sp) = (uint32_t)thread_exit; //lr

    thread->sp = (char*)sp;
    thread->thread_status = RUNNING;
}


//Assumes thread lock held before invocation
static inline bool next_up(cpu_core_e core)
{
    thread_t* thread;

    while(g_cpus[core].incoming_threads->size > 0)
    {
        thread = fifo_pop(g_cpus[core].incoming_threads);

        thread->release_time = g_cpus[core].ticks;
        thread->deadline = g_cpus[core].ticks + thread->period;
        thread->dirty = false;
        thread->thread_status = PENDING;

        thread->owner = DEAD_HEAP;
        insert_node(g_cpus[core].deadHeap, thread, thread->deadline);
    }

    bool thread_found = false;

    while (g_cpus[core].relHeap->curr_index > 0 && geq_wrapped(g_cpus[core].ticks, g_cpus[core].relHeap->heap[0].thread->release_time))
    {
        pop_heap(g_cpus[core].relHeap, &thread);

        thread->owner = DEAD_HEAP;
        insert_node(g_cpus[core].deadHeap, thread, thread->deadline);
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

        if (thread->thread_status == FINISHED)
        {
            if (thread->periodicity == PERIODIC)
            {
                if (thread->period == 0) raise_error("next_up: thread period corrupted");

                do
                {
                    thread->deadline += thread->period;
                }   while (thread->deadline <= g_cpus[core].ticks);

                do
                {
                    thread->release_time += thread->period;
                }   while (thread->release_time < thread->deadline - thread->period);

                thread->dirty = false;
                thread->thread_status = PENDING;

                if (lt_wrapped(g_cpus[core].ticks, thread->release_time))
                {
                    remove_head(g_cpus[core].deadHeap);
                    thread->owner = REL_HEAP;
                    insert_node(g_cpus[core].relHeap, thread, thread->release_time);
                }

                else balance_node(g_cpus[core].deadHeap, thread->heap_index, thread->deadline);
                continue;
            }

            else if (thread->periodicity == APERIODIC)
            {
                remove_head(g_cpus[core].deadHeap);
                thread->owner = OUTGOING_THREADS;
                fifo_push(g_cpus[core].outgoing_threads, thread);
                continue;
            }

            else
            {
                raise_error("next_up: thread periodicity corrupted");
            }
        }

        if(thread->thread_status == PENDING || thread->thread_status == RUNNING)
        {
            if (geq_wrapped(g_cpus[core].ticks, thread->deadline) && !thread->dirty)
            {
                thread->dirty = true;
                g_cpus[core].missed_deadlines++;
            }

            g_cpus[core].curr_thread = thread;
            thread_found = true;
            break;
        }

        else if (thread->thread_status != FINISHED) raise_error("next_up: thread status corrupted");
    }

    if ((g_cpus[core].outgoing_threads->size > 0) && (lock_mutex_best_effort(&g_allocator_mutex) == LOCK_OK))
    {
        while (g_cpus[core].outgoing_threads->size > 0)
        {
            thread_t* thread = fifo_pop(g_cpus[core].outgoing_threads);
            g_cpus[core].utilization -= thread->metrics.prev_u;
            kFree(thread);
        }

        unlock_mutex(&g_allocator_mutex);
    }

    return thread_found;
}





static void thread_exit()
{
    __asm__ __volatile__("cpsid i" ::: "memory");

    cpu_core_e core = curr_core();
    lock_mutex_persistent(&(g_cpus[core].thread_mutex));


    g_cpus[core].curr_thread->thread_status = FINISHED;
    switch_out(g_cpus[core].curr_thread);

    if (!next_up(core)) //Either no valid tasks set or we ran the last one last cycle
    {
        g_cpus[core].curr_thread = g_cpus[core].main_thread;
    }

    switch_in(g_cpus[core].curr_thread);

    switch (g_cpus[core].curr_thread->thread_status)
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
    __asm__ __volatile__("cpsid i" ::: "memory");
    cpu_core_e core = curr_core();
    lock_mutex_persistent(&(g_cpus[core].thread_mutex));

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

    initialize_fifo(g_cpus[core].incoming_threads);
    initialize_fifo(g_cpus[core].outgoing_threads);

    lock_mutex_persistent(&g_allocator_mutex);
    g_cpus[core].main_thread = (thread_t*)kMalloc(sizeof(thread_t));
    unlock_mutex(&g_allocator_mutex);

    if (g_cpus[core].main_thread == NULL) //can't bring up the core without an idle/boot TCB
    {
        unlock_mutex(&(g_cpus[core].thread_mutex));
        __asm__ __volatile__("cpsie i" ::: "memory");
        return SYS_ERROR;
    }

    g_cpus[core].main_thread->thread_status = RUNNING;
    g_cpus[core].main_thread->sp = sp;
    g_cpus[core].main_thread->core = core;
    g_cpus[core].main_thread->periodicity = APERIODIC;
    g_cpus[core].main_thread->id = IDLE_ID;
    init_metrics(g_cpus[core].main_thread);
    g_cpus[core].curr_thread = g_cpus[core].main_thread;
    g_cpus[core].sched_init = true;
    g_cpus[core].request_terminate = false;

    unlock_mutex(&(g_cpus[core].thread_mutex));

    __asm__ __volatile__("dmb sy" ::: "memory");
    __asm__ __volatile__("cpsie i" ::: "memory");

    return SYS_OK;
}



sys_exit_e psched_core_deinit(void)
{
    __asm__ __volatile__("cpsid i" ::: "memory");
    cpu_core_e core = curr_core();

    lock_mutex_persistent(&(g_cpus[core].thread_mutex));
    g_cpus[core].request_terminate = true;
    unlock_mutex(&(g_cpus[core].thread_mutex));

    __asm__ __volatile__("dmb sy" ::: "memory");
    __asm__ __volatile__("cpsie i" ::: "memory");
    
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

    uint32_t irq_state;
    __asm__ __volatile__("mrs %0, cpsr\n\t"
                         "cpsid i"
                         : "=r"(irq_state) :: "memory");

    lock_mutex_persistent(&g_allocator_mutex);
    thread_t* new_thread = (thread_t*)kMalloc(sizeof(thread_t));
    unlock_mutex(&g_allocator_mutex);

    if (new_thread == NULL) //heap exhausted; restore caller's IRQ state and report failure
    {
        __asm__ __volatile__("msr cpsr_c, %0" :: "r"(irq_state) : "memory");
        return NULL;
    }

    new_thread->period = period;
    new_thread->periodicity = periodicity;
    new_thread->id = id;
    new_thread->owner = INCOMING_THREADS;

    new_thread->func = func;
    new_thread->sp = (char*)(((uintptr_t)(new_thread->stack + THREAD_STACK_SIZE)) & ~(uintptr_t)0x7); //8-byte align sp so processor doesn't abort

    new_thread->core = core;
    init_metrics(new_thread);

    lock_mutex_persistent(&(g_cpus[core].thread_mutex));
    fifo_push(g_cpus[core].incoming_threads, new_thread);
    unlock_mutex(&(g_cpus[core].thread_mutex));

    __asm__ __volatile__("msr cpsr_c, %0" :: "r"(irq_state) : "memory");


    return new_thread;
}



thread_t* add_thread(sys_exit_e (*func)(void), uint32_t period, thread_periodicity_e periodicity, uint32_t id)
{
    static cpu_core_e tie_default = CPU0;

    cpu_core_e core;
    if (g_cpus[CPU0].utilization < g_cpus[CPU1].utilization) core = CPU0;
    else if (g_cpus[CPU1].utilization < g_cpus[CPU0].utilization) core = CPU1;
    else { core = tie_default; tie_default = (tie_default == CPU0) ? CPU1 : CPU0; }

    return enqueue_new_thread(core, func, period, periodicity, id);
}



thread_t* add_thread_to_core(cpu_core_e core, sys_exit_e (*func)(void), uint32_t period, thread_periodicity_e periodicity, uint32_t id)
{
    return enqueue_new_thread(core, func, period, periodicity, id);
}



sys_exit_e kill_thread(thread_t* thread)
{
    uint32_t irq_state;
    __asm__ __volatile__("mrs %0, cpsr\n\t"
                         "cpsid i"
                         : "=r"(irq_state) :: "memory");

    lock_mutex_persistent(&(g_cpus[thread->core].thread_mutex));

    thread->periodicity = APERIODIC;
    thread->thread_status = FINISHED;
    thread->owner = OUTGOING_THREADS;
    g_cpus[thread->core].utilization -= thread->metrics.prev_u;
    thread->metrics.prev_u = 0;
    unlock_mutex(&(g_cpus[thread->core].thread_mutex));

    __asm__ __volatile__("msr cpsr_c, %0" :: "r"(irq_state) : "memory");

    return SYS_OK;
}



//TODO: Priority inheritance mechanism is necessary here to guarantee thread mutex deadlock will not happen!
//At the moment is ok because cpsid used before every attempt to lock thread mutex but it's not mechanistically enforced


sys_exit_e suspend_thread(thread_t* thread, uint32_t delay)
{
    uint32_t irq_state;

    cpu_core_e core = thread->core;
    cpu_core_e currCore = curr_core();

    __asm__ __volatile__("mrs %0, cpsr\n" //TODO: See if this should be changed to cpsr_c
                         "cpsid i\n"

                         : "=r"(irq_state)  //= denotes write-only, r denotes general purpose register
                         :
                         : "memory");


    lock_mutex_persistent(&(g_cpus[core].thread_mutex));

    sys_exit_e ret = SYS_OK;

    if ((thread->thread_status == FINISHED && thread->periodicity == APERIODIC) || thread->owner == OUTGOING_THREADS || thread->owner == INCOMING_THREADS) ret = SYS_ERROR; //Can't suspend something that's finished and won't run again

    if (ret == SYS_OK)
    {
        uint32_t ticks = g_cpus[core].ticks;
        if (geq_wrapped(thread->release_time, ticks + delay)) ret = SYS_ERROR; //Can't run until at least delay ticks anyway

        if (ret == SYS_OK)
        {
            thread->release_time = ticks + delay;

            if (geq_wrapped(ticks, thread->deadline)) //If past deadline start clean
            {
                thread->deadline = thread->release_time + thread->period;
            }

            else 
            {
                uint32_t delta = thread->deadline - ticks;
                thread->deadline = thread->release_time + delta;
            }

            if (thread->owner == DEAD_HEAP)
            {
                thread->owner = REL_HEAP;
                remove_node(g_cpus[core].deadHeap, thread->heap_index);
                insert_node(g_cpus[core].relHeap, thread, thread->release_time);
            }

            else if(thread->owner == REL_HEAP)
            {
                balance_node(g_cpus[core].relHeap, thread->heap_index, thread->release_time);
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

        unlock_mutex(&(g_cpus[core].thread_mutex));

        SWITCH_FRAME_AND_RETURN(new_sp);
        __builtin_unreachable();
    }


    else unlock_mutex(&(g_cpus[core].thread_mutex));


    resume:
        __asm__ __volatile__("msr cpsr_c, %0" :: "r"(irq_state) : "memory");
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

            switch (g_cpus[core].curr_thread->thread_status)
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
