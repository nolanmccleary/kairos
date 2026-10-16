#include "timers.h"


void cpu0_timer_start(void)
{
    GTIMER_CTRL    = 0;
    GTIMER_ISR     = 1;
    GTIMER_AUTOINC = 199999;
    GTIMER_CMPL    = GTIMER_CNTRL + 199999;
    GTIMER_CMPH    = GTIMER_CNTRH;
    GTIMER_CTRL    = (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0);
}

void cpu1_timer_start(void)
{
    GTIMER_ISR     = 1;
    GTIMER_AUTOINC = 199999;
    GTIMER_CMPL    = GTIMER_CNTRL + 199999;
    GTIMER_CMPH    = GTIMER_CNTRH;
    GTIMER_CTRL   |= (1 << 3) | (1 << 2) | (1 << 1);
}

