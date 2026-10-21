#ifndef __MIN_HEAP_H_
#define __MIN_HEAP_H_


#include <stddef.h>
#include <stdint.h>
#include "thread.h"


typedef enum
{
    OP_OK,
    OP_FAILED,
}   heap_op_e;


typedef enum 
{
    TYPE_THREAD,
    TYPE_ISR,
}   heap_type_e;



typedef struct
{
    uint32_t order;
    heap_type_e type;
    void* item;
}   heap_node_t;


typedef struct
{
    heap_node_t heap [MAX_THREADS];
    size_t curr_index;
}   heap_t;


heap_op_e min_heap_insert_node(heap_t* heap, void* item, uint32_t order, heap_type_e type);
heap_op_e min_heap_remove_head(heap_t* heap);
heap_op_e min_heap_remove_node(heap_t* heap, size_t index);
heap_op_e min_heap_balance_node(heap_t* heap, size_t index, uint32_t order);
heap_op_e min_heap_pop_heap(heap_t* heap, void** item);
heap_op_e max_heap_insert_node(heap_t* heap, void* item, uint32_t order, heap_type_e type);
heap_op_e max_heap_remove_head(heap_t* heap);
heap_op_e max_heap_remove_node(heap_t* heap, size_t index);
heap_op_e max_heap_balance_node(heap_t* heap, size_t index, uint32_t order);
heap_op_e max_heap_pop_heap(heap_t* heap, void** item, heap_type_e type);

#endif

