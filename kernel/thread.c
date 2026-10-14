#include "thread.h"
#include "pmu.h"
#include "aux.h"




void init_metrics(thread_t* thread)
{
    thread->metrics = (metrics_t){0};
    return;
}


//TODO: Evaluate whether inlining improves scheduler overhead
void switch_in(thread_t *thread)
{
    uint32_t curr_cycles = pmu_cycles();

    switch (thread->md.status)
    {
        case PENDING:
            uint32_t ti_curr = curr_cycles - thread->metrics.t0;
            thread->metrics.ti = ti_curr;
            thread->metrics.ti_av = thread->metrics.ti_av - (thread->metrics.ti_av >> ALPHA) + (ti_curr >> ALPHA);
            thread->metrics.t0 = curr_cycles;

            thread->metrics.delta_sum = 0;
            //fallthrough


        case RUNNING:
            thread->metrics.prev_cycles = curr_cycles;
            break;


        case FINISHED: //should never switch in in this state
        default: 
            break;
    }

    return;
}



void switch_out(thread_t* thread)
{
    uint32_t curr_cyles = pmu_cycles();
    thread->metrics.delta_sum += curr_cyles - thread->metrics.prev_cycles; 

    switch (thread->md.status)
    {
        case RUNNING:
            break;

        case FINISHED:
            uint32_t ci = thread->metrics.delta_sum;
            thread->metrics.ci = ci;
            thread->metrics.ci_av = thread->metrics.ci_av - (thread->metrics.ci_av >> ALPHA) + (ci >> ALPHA);
            break;

        default: 
            break;
    }
}

