#include <stdint.h>
#include "bsp.h"
#include "preempt_sched.h"
#include "tlsf.h"
#include "pmu.h"
#include "ktrace.h"








void cpu1_main(void)
{
    for (;;){};
}



void allocbench_run(void);
void rmw_run(void);
void matmul_run(void);
void edf_run(void);
void edf_balance_run(void);
void micro_run(void);
void suspend_run(void);

void main(void)
{

#if defined(MODE_TEST)
    __asm__ __volatile__("cpsid if" ::: "memory");

    // allocbench_run();   /* slots 0-3 -- swapped out to make OCRAM room for suspend_run */
    // rmw_run();          /* slots 4-5 -- swapped out */
    // matmul_run();       /* slot 6   -- swapped out */


    // KTRACE_ALLOC_DONE();

    edf_run();
    edf_balance_run();
    // micro_run();   /* swapped out earlier */
    suspend_run();
#endif

    for (;;) { }
}
