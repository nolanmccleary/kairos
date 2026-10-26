#include "lock.h"
#include "cpu.h"
#include "sched.h"


volatile uint32_t g_spin_exit;


#if ENABLE_SMP


extern void         _lock_mutex_persistent(void* mutex);
extern mutex_lock_e _lock_mutex_best_effort(void* mutex);
extern void         _unlock_mutex(void* mutex);


void lock_mutex_persistent(mutex_t* mutex)
{
    _lock_mutex_persistent(mutex);
}


mutex_lock_e lock_mutex_best_effort(mutex_t* mutex)
{
    return _lock_mutex_best_effort(mutex);
}


void unlock_mutex(mutex_t* mutex)
{
    _unlock_mutex(mutex);
}

#else


void lock_mutex_persistent(mutex_t* mutex)
{
    *(volatile uint32_t*)mutex = 1u;
}


mutex_lock_e lock_mutex_best_effort(mutex_t* mutex)
{
    if (*(volatile uint32_t*)mutex != 0u)
    {
        return LOCK_FAIL;
    }
    *(volatile uint32_t*)mutex = 1u;
    return LOCK_OK;
}


void unlock_mutex(mutex_t* mutex)
{
    *(volatile uint32_t*)mutex = 0u;
}

#endif


mutex_lock_e lock_mutex_timeout(mutex_t* mutex, uint32_t timeout)
{
    mutex_lock_e lock = lock_mutex_best_effort(mutex);

    if (lock == LOCK_FAIL)
    {
        cpu_core_e core = curr_core();
        suspend_thread(g_cpus[core].curr_thread, timeout); //Note that this will have variable latency based on if the core lock is currently held
    }

    return lock;
}
