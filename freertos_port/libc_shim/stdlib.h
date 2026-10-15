#ifndef FRTOS_SHIM_STDLIB_H
#define FRTOS_SHIM_STDLIB_H
/* Minimal shim: this arm-none-eabi toolchain is gcc-only (no newlib). FreeRTOS
 * heap_4.c includes <stdlib.h> but only needs size_t from it. */
#include <stddef.h>
#endif
