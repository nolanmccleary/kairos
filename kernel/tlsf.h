#ifndef __TLSF_H__
#define __TLSF_H__

#include <stddef.h>
#include "lock.h"


typedef enum
{
    ALLOC_OP_OK,
    ALLOC_OP_FAIL,
}   allocator_op_e;


extern volatile bool g_heap_initialized;
extern mutex_t g_allocator_mutex;


allocator_op_e  heap_init(void);
allocator_op_e  heap_destroy(void);
allocator_op_e  heap_reset(void);
void*           kMalloc(size_t size);
allocator_op_e  kFree(void* target);

#endif

