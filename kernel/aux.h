#ifndef __AUX_H__
#define __AUX_H__

#include <stdbool.h>
#include <stdint.h>

#define ALPHA 2


static inline bool gt_wrapped(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b) > 0);
}


static inline bool geq_wrapped(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b) >= 0);
}


static inline bool lt_wrapped(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b) < 0);
}


#endif

