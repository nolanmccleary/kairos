#include <stdbool.h>
#include "aux.h"
#include "min_heap.h"
#include "thread.h"




static inline int find_parent_index(int pos)
{
    if (pos == 0) return 0;
    else return (pos-1) / 2;
}

static inline int find_left_child(int pos)                                                                
{
    return pos * 2 + 1;
}

static inline int find_right_child(int pos)
{
    return 2 * (pos + 1);
}

static inline int in_range(int curr_index, int index)
{
    return index < curr_index;
}


//TODO: Make this iterative at some point
static inline void percolate_up(heap_t* heap, int index)
{
    int parent_index = find_parent_index(index);

    if((index == 0) || geq_wrapped(heap->heap[index].order, heap->heap[parent_index].order)) return;
    
    else
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = heap->heap[parent_index];
        heap->heap[parent_index] = tmp;

        heap->heap[index].thread->md.heap_index = index;
        heap->heap[parent_index].thread->md.heap_index = parent_index;

        return percolate_up(heap, parent_index);
    }       
}


static inline void percolate_down(heap_t* heap, int index)
{
    int left_child_index = find_left_child(index);
    int right_child_index = find_right_child(index);
    
    bool swappable = false;
    bool swappable_right = false;

    heap_node_t min_order_node = heap->heap[index];

    if(in_range(heap->curr_index, left_child_index))
    {
        swappable = lt_wrapped(heap->heap[left_child_index].order, min_order_node.order);
        if(swappable) min_order_node = heap->heap[left_child_index];
    }

    if(in_range(heap->curr_index, right_child_index))
    {
        swappable_right = lt_wrapped(heap->heap[right_child_index].order, min_order_node.order);
        if(swappable_right) min_order_node = heap->heap[right_child_index];
    }

    if(swappable_right)
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = min_order_node;
        heap->heap[right_child_index] = tmp;

        heap->heap[index].thread->md.heap_index = index;
        heap->heap[right_child_index].thread->md.heap_index = right_child_index;

        return percolate_down(heap, right_child_index);
    }

    else if(swappable)
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = min_order_node;
        heap->heap[left_child_index] = tmp;

        heap->heap[index].thread->md.heap_index = index;
        heap->heap[left_child_index].thread->md.heap_index = left_child_index;

        return percolate_down(heap, left_child_index);
    }


    else return;
}


heap_op_e insert_node(heap_t* heap, thread_t* thread, uint32_t order)
{
    
    if (heap->curr_index < MAX_THREADS)
    {
        heap_node_t node;
        node.thread = thread;
        node.order = order;

        heap->heap[heap->curr_index] = node;
        node.thread->md.heap_index = heap->curr_index;

        percolate_up(heap, heap->curr_index++);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e remove_head(heap_t* heap)
{
    if (heap->curr_index > 0)
    {
        heap->heap[0] = heap->heap[--heap->curr_index];
        heap->heap[0].thread->md.heap_index = 0;

        percolate_down(heap, 0);
        return OP_OK;
    }

    else return OP_FAILED;
}



heap_op_e remove_node(heap_t* heap, size_t index)
{
    if (heap->curr_index > 0)
    {
        heap->heap[index] = heap->heap[--heap->curr_index];
        heap->heap[index].thread->md.heap_index = index;

        size_t parent_index = find_parent_index(index);
        if (heap->heap[index].order < heap->heap[parent_index].order) percolate_up(heap, index);
        else percolate_down(heap, index);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e balance_node(heap_t* heap, size_t index, uint32_t order)
{
    if (heap->curr_index > 0)
    {
        size_t parent_index = find_parent_index(index);
        heap->heap[index].order = order;
        if (heap->heap[index].order < heap->heap[parent_index].order) percolate_up(heap, index);
        else percolate_down(heap, index);
        return OP_OK;
    }

    else return OP_FAILED;
}




heap_op_e pop_heap(heap_t* heap, thread_t** thread)
{
    if (heap->curr_index > 0)
    {
        heap_node_t node = heap->heap[0];
        if(remove_head(heap) == OP_FAILED) return OP_FAILED;

        *thread = node.thread;
        return OP_OK;
    }

    else
    {
        *thread = NULL;
        return OP_FAILED;
    }
}

