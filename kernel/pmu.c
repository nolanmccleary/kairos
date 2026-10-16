#include "pmu.h"

/* PMCR bits (CP15 c9,c12,0). */
#define PMCR_E (1u << 0) /* enable all counters      */
#define PMCR_C (1u << 2) /* reset cycle counter      */
#define PMCR_D (1u << 3) /* cycle count divider (/64)*/

/* PMCNTENSET bit 31 = cycle counter (CP15 c9,c12,1). */
#define PMCNTEN_CCNT (1u << 31)


void pmu_init(void)
{
    uint32_t v;

    /* PMCR: enable + reset the cycle counter, divider off (count every cycle). */
    __asm__ __volatile__("mrc p15, 0, %0, c9, c12, 0" : "=r"(v));
    v |=  (PMCR_E | PMCR_C);
    v &= ~PMCR_D;
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 0" : : "r"(v));

    /* Enable the cycle counter itself. */
    v = PMCNTEN_CCNT;
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 1" : : "r"(v));

    __asm__ __volatile__("isb" ::: "memory");
}

