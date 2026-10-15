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
void persistent_run(void);
void allocsweep_run(void);
void fragsweep_run(void);
void schedsweep_run(void);
#if USE_FREERTOS
void freertos_main(void);
#endif

void main(void)
{

#if defined(MODE_TEST)
    // __asm__ __volatile__("cpsid if" ::: "memory");

#if USE_FREERTOS
    /* FreeRTOS battery: start the scheduler and run every bench as a FreeRTOS task
     * (never returns). */
    freertos_main();
#else
    allocbench_run();   /* slots 0-3 */
    rmw_run();          /* slots 4-5 */
    matmul_run();       /* slot 6   */

    allocsweep_run();   /* TLSF alloc/free cost vs payload size */
    fragsweep_run();    /* TLSF alloc/free cost vs free-list length (fragmentation) */

    KTRACE_ALLOC_DONE();

    edf_run();
    edf_balance_run();
    micro_run();
    suspend_run();
    persistent_run();
    schedsweep_run();   /* scheduler overhead vs number of loaded tasks */
#endif
#endif

    for (;;) { }
}
