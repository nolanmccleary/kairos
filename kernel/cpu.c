#include "gic.h"
#include "cpu.h"
#include "aux.h"
#include "telemetry.h"


HOST_SHARED_OCRAM volatile uint32_t g_cpu_mailbox_uncached;


cpu_t g_cpus[NUM_CPUS];

//TODO: Rename thread mutex ot core mutex

inline cpu_core_e curr_core(void)
{
    uint32_t mpidr;
    __asm__ volatile ("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return (cpu_core_e)(mpidr & 0xFF);
}


inline void update_cpu_scheduler_overhead(cpu_core_e cpu, uint32_t overhead)
{
    g_cpus[cpu].avg_scheduler_overhead = overhead - (overhead >> ALPHA) + (g_cpus[cpu].avg_scheduler_overhead >> ALPHA);
}


void update_utilization(thread_t* thread)
{
    cpu_core_e core = thread->core;

    if (thread->metrics.ti_av > 0 && thread->md.periodicity == PERIODIC)
    {
        uint32_t u = (uint32_t)(((uint64_t)thread->metrics.ci_av << 16) / thread->metrics.ti_av);
        g_cpus[core].utilization -= thread->metrics.prev_u;
        g_cpus[core].utilization += u;
        thread->metrics.prev_u = u;
    }
}

static cpu_core_e tie_default = CPU0;
inline cpu_core_e least_loaded_core(void)
{

    if (g_cpus[CPU0].utilization < g_cpus[CPU1].utilization) return CPU0;
    if (g_cpus[CPU1].utilization < g_cpus[CPU0].utilization) return CPU1;

    cpu_core_e core = tie_default;
    tie_default = (tie_default == CPU0) ? CPU1 : CPU0;
    return core;
}


inline void send_cpu_interrupt(cpu_sgi_e interrupt)
{
    cpu_core_e core = curr_core();
    __asm__ volatile ("dmb sy" ::: "memory");
    GICD_SGIR = (1u << (16u + (core ^ 0x1u)))   // CPUTargetList [23:16]: the other core
              | ((uint32_t)interrupt & 0xFu);    // SGI ID [3:0]
}

