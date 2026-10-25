#include <stddef.h>
#include <stdint.h>

#include "memset.h"



void* memset(void* dest, int fill_value, size_t num_bytes)
{
    uint8_t fill_byte = (uint8_t)fill_value;

    //To next word boundary
    uint8_t* byte_ptr = (uint8_t*)dest;

    while (num_bytes && ((uintptr_t)byte_ptr & 0b11))
    {
        *byte_ptr++ = fill_byte;
        num_bytes--;
    }

    size_t words_remaining = num_bytes >> 2;
    size_t bytes_remaining = num_bytes % 4;

    uint32_t fill_word = (uint32_t)fill_byte * 0x01010101u;
    uint32_t* word_ptr = (uint32_t*)byte_ptr;

    while (words_remaining)
    {
        *word_ptr++ = fill_word;
        words_remaining--;
    }

    uint8_t* tail_ptr = (uint8_t*)word_ptr;
    while (bytes_remaining)
    {
        *tail_ptr++ = fill_byte;
        bytes_remaining--;
    }

    return dest;
}
