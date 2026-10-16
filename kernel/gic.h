#ifndef __GIC_H__
#define __GIC_H__

#include <stdint.h>
#include "board.h"


#define GICD_BASE       BOARD_GICD_BASE

#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000u))
#define GICD_ISENABLER0 (*(volatile uint32_t *)(GICD_BASE + 0x100u))
#define GICD_SGIR       (*(volatile uint32_t *)(GICD_BASE + 0xF00u))
#define GICC_CTLR       (*(volatile uint32_t *)(BOARD_GICC_BASE + 0x00u))
#define GICC_PMR        (*(volatile uint32_t *)(BOARD_GICC_BASE + 0x04u))


void cpu0_gic_init(void);
void cpu1_gic_init(void);


#endif

