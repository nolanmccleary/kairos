#ifndef __AUX_H__
#define __AUX_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ALPHA 2


#define OCRAM_UNCACHED_MAILBOX __attribute__((section(".ocram_uncached_mailbox"), used))



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


static inline bool leq_wrapped(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b) <= 0);
}


static inline void memzero(void* targ, size_t num_bytes)
{
    //To next word boundary 
    uint8_t* t1 = (uint8_t*)targ;

    while (num_bytes && ((uintptr_t)t1 & 0b11))
    {
        *t1++ = 0;
        num_bytes--;
    }

    //All remaining words and trailing bytes 
    size_t words_remaining = num_bytes >> 2;
    size_t bytes_remaning = num_bytes % 4;
    
    uint32_t* t2 = (uint32_t*)t1;

    //Trailing bytes
    while (words_remaining)
    {
        *t2++ = 0;
        words_remaining--;
    }

    uint8_t* t3 = (uint8_t*)t2;
    while (bytes_remaning)
    {
        *t3++ = 0;
        bytes_remaning--;
    }

    return;
}


#endif

