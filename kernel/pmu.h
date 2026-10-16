#ifndef __PMU_H__
#define __PMU_H__

#include <stdint.h>


void pmu_init(void);


static inline uint32_t pmu_cycles(void)
{
    uint32_t c;
    __asm__ __volatile__("isb\n\t"
                         "mrc p15, 0, %0, c9, c13, 0\n\t" //Sys control coprocessor, null opc1 sel, gcc choose targ reg, PMU reg bank, secondary reg sel is c(oprocessor reg)13
                         : "=r"(c)
                         :
                         : "memory");
    return c;
}


#endif

