#ifndef __IRQ_H__
#define __IRQ_H__

#include <stdint.h>


typedef union
{
    struct
    {
        uint32_t sgi_id            : 4;  // [3:0]
        uint32_t reserved0         : 12; // [15:4]
        uint32_t cpu_target_list   : 8;  // [23:16]
        uint32_t target_list_filter: 2;  // [25:24]
        uint32_t reserved1         : 6;  // [31:26]
    }   bits;

        uint32_t raw;
}       gicd_sgir_t;


typedef enum
{
    CPU_PSCHED_INIT_REQUEST,
    CPU_PSCHED_INIT_ACK,
    CPU_PSCHED_DEINIT_REQUEST,
    CPU_PSCHED_DEINIT_ACK,
    CPU_IRQ_PING,
    CPU_MUTEX_HOLD,
}   cpu_sgi_e;


typedef enum
{
    GICD_IRQ = 0x1b,
}   hardware_irq_e;


#endif

