#ifndef __THREAD_FIFO_H__
#define __THREAD_FIFO_H__


#include "thread.h"
#include <stdint.h>
#include <stddef.h>


typedef enum
{
    FIFO_OK,
    FIFO_FAIL,
}   fifo_op_e;


typedef struct
{
    _Alignas(32) size_t start;
    size_t end;
    size_t size;

    thread_t* fifo [MAX_THREADS];
}   thread_fifo_t;


void initialize_fifo(thread_fifo_t* fifo);
void destroy_threads(thread_fifo_t* fifo);

fifo_op_e fifo_push(thread_fifo_t* fifo, thread_t* thread);
thread_t* fifo_pop(thread_fifo_t* fifo);
thread_t* fifo_remove(thread_fifo_t* fifo, size_t index);

#endif

