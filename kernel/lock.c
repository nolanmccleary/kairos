#include "lock.h"
#include "cpu.h"
#include "preempt_sched.h"


volatile uint32_t g_spin_exit;


extern void _lock_mutex_persistent(void* mutex);
extern mutex_lock_e _lock_mutex_best_effort(void* mutex);
extern void _unlock_mutex(void* mutex);


void lock_mutex_persistent(mutex_t *mutex)
{
    _lock_mutex_persistent(mutex);
    (void)(0);
}


mutex_lock_e lock_mutex_best_effort(mutex_t *mutex)
{
    return _lock_mutex_best_effort(mutex);
}


mutex_lock_e lock_mutex_timeout(mutex_t* mutex, uint32_t timeout)
{
    mutex_lock_e lock = _lock_mutex_best_effort(mutex);

    if (lock == LOCK_FAIL)
    {
        cpu_core_e core = curr_core();
        suspend_thread(g_cpus[core].curr_thread, timeout); //Note that this will have variable latency based on if the core lock is currently held
    }

    return lock;
}



void unlock_mutex(mutex_t* mutex)
{
    _unlock_mutex(mutex);
    (void)(0);
}
