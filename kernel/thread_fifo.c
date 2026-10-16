#include "thread_fifo.h"
#include "tlsf.h"


inline size_t inc_wrap(size_t n)
{
    return (n + 1) % MAX_THREADS;
}


void initialize_fifo(thread_fifo_t* fifo)
{
    fifo->start = 0;
    fifo->end = 0;
    fifo->size = 0;
    return;
}


void destroy_threads(thread_fifo_t* fifo)
{
    for (size_t i = 0; i < MAX_THREADS; i++)
    {
        if (fifo->fifo[i] != NULL) kFree(fifo->fifo[i]);
        fifo->fifo[i] = NULL;
    }

    fifo->start = 0;
    fifo->end = 0;
    fifo->size = 0;

    return;
}


fifo_op_e fifo_push(thread_fifo_t* fifo, thread_t* thread)
{
    if (fifo->size < MAX_THREADS)
    {
        fifo->size++;
        fifo->fifo[fifo->end] = thread;
        fifo->end = inc_wrap(fifo->end);
        return FIFO_OK;
    }

    return FIFO_FAIL;
}


thread_t* fifo_pop(thread_fifo_t* fifo)
{
    if (fifo->size != 0)
    {
        thread_t* ret = fifo->fifo[fifo->start];
        fifo->fifo[fifo->start] = NULL;
        fifo->size--;
        fifo->start = inc_wrap(fifo->start);
        return ret;
    }

    return NULL;
}

