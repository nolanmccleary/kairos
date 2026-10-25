#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#include "pmu.h"


#define SDRAM_UNCACHED_TELEMETRY       __attribute__((section(".sdram_uncached_telemetry"), used))
#define OCRAM_UNCACHED_TELEMETRY __attribute__((section(".ocram_uncached_telemetry"), used))



typedef struct
{
    uint32_t min;
    uint32_t max;
    uint32_t sum;     /* mean = sum / count */
    uint32_t count;
}   metric_t;


#define METRIC_SLOTS  8   /* slots 0..6 used: 0-3 allocbench, 4-5 rmw, 6 matmul.
                             slot 7 now unused (scheduler cost moved to per-CPU
                             g_cpus[].avg_overhead) */


extern metric_t g_metrics[METRIC_SLOTS];


static inline void metric_reset(int slot)
{
    g_metrics[slot].min   = 0xFFFFFFFFu;
    g_metrics[slot].max   = 0u;
    g_metrics[slot].sum   = 0u;
    g_metrics[slot].count = 0u;
}


static inline void metric_add(int slot, uint32_t cycles)
{
    metric_t* m = &g_metrics[slot];

    if (cycles < m->min) m->min = cycles;
    if (cycles > m->max) m->max = cycles;

    m->sum   += cycles;
    m->count += 1u;
}


/* Bracket a region; MEASURE_END folds the elapsed cycle count into slot `id`. */
#define MEASURE_BEGIN(id) \
    uint32_t _mt_##id = pmu_cycles()

#define MEASURE_END(id) \
    metric_add((id), pmu_cycles() - _mt_##id)

/* Like MEASURE_END, but also stores the raw per-iteration delta into `dst` (any
 * lvalue) so the host can plot the full distribution, not just min/max. */
#define MEASURE_END_INTO(id, dst) \
    do { \
        uint32_t _dt_##id = pmu_cycles() - _mt_##id; \
        metric_add((id), _dt_##id); \
        (dst) = _dt_##id; \
    } while (0)


#endif
