#ifndef __LOCK_H__
#define __LOCK_H__

#include <stdint.h>


extern volatile uint32_t g_spin_exit;


typedef uint32_t mutex_t;


typedef enum : uint32_t
{
    LOCK_OK = 0,
    LOCK_FAIL = 1,
}   mutex_lock_e;


void lock_mutex_persistent(mutex_t* mutex);
mutex_lock_e lock_mutex_best_effort(mutex_t* mutex);
void unlock_mutex(mutex_t* mutex);


#endif

