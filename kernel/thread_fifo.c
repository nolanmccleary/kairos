#include "thread_fifo.h"
#include "tlsf.h"


inline size_t inc_wrap(size_t n)
{
    return (n + 1) % MAX_THREADS;
}

inline size_t dec_wrap(size_t n)
{
    return (n == 0)? MAX_THREADS-1 : n-1;
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
        if (fifo->fifo[i] != NULL) fifo->fifo[i]->md.owner = REMOVED;
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
        thread->md.index = fifo->end;
        fifo->end = inc_wrap(fifo->end);
        return FIFO_OK;
    }

    return FIFO_FAIL;
}


thread_t* fifo_pop(thread_fifo_t* fifo)
{
    if (fifo->size > 0)
    {
        thread_t* ret = fifo->fifo[fifo->start];
        fifo->fifo[fifo->start] = NULL;
        fifo->size--;
        fifo->start = inc_wrap(fifo->start);
        return ret;
    }

    return NULL;
}


thread_t* fifo_remove(thread_fifo_t* fifo, size_t index) //O(n) bounded to max entries and runtime 
                                                         //is not important for this func anyway.
                                                         //Assume index is valid
{
    if (fifo->size > 0)
    {
        thread_t* ret = fifo->fifo[index];

        size_t i = index;
        size_t j = inc_wrap(i);

        while (j != fifo->end)
        {
            fifo->fifo[i] = fifo->fifo[j];
            fifo->fifo[i]->md.index = dec_wrap(fifo->fifo[i]->md.index);
            i = j;
            j = inc_wrap(j);
        } 

        fifo->size--;
        fifo->end = dec_wrap(fifo->end);
        fifo->fifo[fifo->end] = NULL;
        return ret;
    }

    return NULL;
}
