/* Kairos scheduler public interface: a hybrid EDF + round-robin policy. */
#ifndef __SCHED_H__
#define __SCHED_H__

#include <stdint.h>
#include "system.h"
#include "thread.h"
#include "cpu.h"


sys_exit_e psched_core_init(void);
sys_exit_e psched_core_deinit(void);
sys_exit_e psched_init(void);
sys_exit_e psched_deinit(void);
sys_exit_e psched_clear_threads(void);


thread_t* add_periodic_thread(thread_t* thread, sys_exit_e (*func)(void), uint32_t period, thread_periodicity_e periodicity, uint32_t id);
thread_t* add_periodic_thread_to_core(thread_t* thread, cpu_core_e core, sys_exit_e (*func)(void), uint32_t period, thread_periodicity_e periodicity, uint32_t id);

thread_t* add_persistent_thread(thread_t* thread, sys_exit_e (*func)(void), uint32_t id, persistent_thread_priority_e priority);
thread_t* add_persistent_thread_to_core(thread_t* thread, cpu_core_e core, sys_exit_e (*func)(void), uint32_t id, persistent_thread_priority_e priority);


sys_exit_e kill_thread(thread_t* thread); //assumed called from same core for now

sys_exit_e suspend_thread(thread_t* thread, uint32_t delay);


void next_thread(void);


#endif

