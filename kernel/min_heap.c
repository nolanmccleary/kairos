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

static inline void set_item_index(heap_t* heap, int index)
{
    if (heap->heap[index].type == TYPE_THREAD)
        ((thread_t*)heap->heap[index].item)->md.heap_index = index;
}


static inline void min_heap_percolate_up(heap_t* heap, int index)
{
    int parent_index = find_parent_index(index);

    if((index == 0) || geq_wrapped(heap->heap[index].order, heap->heap[parent_index].order)) return;

    else
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = heap->heap[parent_index];
        heap->heap[parent_index] = tmp;

        set_item_index(heap, index);
        set_item_index(heap, parent_index);

        return min_heap_percolate_up(heap, parent_index);
    }
}


static inline void min_heap_percolate_down(heap_t* heap, int index)
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

        set_item_index(heap, index);
        set_item_index(heap, right_child_index);

        return min_heap_percolate_down(heap, right_child_index);
    }

    else if(swappable)
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = min_order_node;
        heap->heap[left_child_index] = tmp;

        set_item_index(heap, index);
        set_item_index(heap, left_child_index);

        return min_heap_percolate_down(heap, left_child_index);
    }


    else return;
}


heap_op_e min_heap_insert_node(heap_t* heap, void* item, uint32_t order, heap_type_e type)
{

    if (heap->curr_index < MAX_THREADS)
    {
        heap_node_t node;
        node.item = item;
        node.order = order;
        node.type = type;

        heap->heap[heap->curr_index] = node;
        set_item_index(heap, heap->curr_index);

        min_heap_percolate_up(heap, heap->curr_index++);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e min_heap_remove_head(heap_t* heap)
{
    if (heap->curr_index == 1)
    {
        heap->curr_index--;
        return OP_OK;
    }

    else if (heap->curr_index > 1)
    {
        heap->heap[0] = heap->heap[--heap->curr_index];
        set_item_index(heap, 0);

        min_heap_percolate_down(heap, 0);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e min_heap_remove_node(heap_t* heap, size_t index)
{
    if (index >= heap->curr_index) return OP_FAILED;

    else if (heap->curr_index == 1)
    {
        heap->curr_index--;
        return OP_OK;
    }

    else if (heap->curr_index > 1)
    {
        heap->heap[index] = heap->heap[--heap->curr_index];
        set_item_index(heap, index);

        size_t parent_index = find_parent_index(index);
        if (heap->heap[index].order < heap->heap[parent_index].order) min_heap_percolate_up(heap, index);
        else min_heap_percolate_down(heap, index);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e min_heap_balance_node(heap_t* heap, size_t index, uint32_t order)
{
    if (heap->curr_index == 1) return OP_OK;

    else if (heap->curr_index > 1)
    {
        size_t parent_index = find_parent_index(index);
        heap->heap[index].order = order;
        if (heap->heap[index].order < heap->heap[parent_index].order) min_heap_percolate_up(heap, index);
        else min_heap_percolate_down(heap, index);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e min_heap_pop_heap(heap_t* heap, void** item)
{
    if (heap->curr_index > 0)
    {
        heap_node_t node = heap->heap[0];
        if(min_heap_remove_head(heap) == OP_FAILED) return OP_FAILED;

        *item = node.item;
        return OP_OK;
    }

    else
    {
        *item = NULL;
        return OP_FAILED;
    }
}












/////////////////////////// MAX HEAP SECTION //////////////////////////////////


static inline void max_heap_percolate_up(heap_t* heap, int index)
{
    int parent_index = find_parent_index(index);

    if((index == 0) || leq_wrapped(heap->heap[index].order, heap->heap[parent_index].order)) return;

    else
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = heap->heap[parent_index];
        heap->heap[parent_index] = tmp;

        set_item_index(heap, index);
        set_item_index(heap, parent_index);

        return max_heap_percolate_up(heap, parent_index);
    }
}


static inline void max_heap_percolate_down(heap_t* heap, int index)
{
    int left_child_index = find_left_child(index);
    int right_child_index = find_right_child(index);

    bool swappable = false;
    bool swappable_right = false;

    heap_node_t min_order_node = heap->heap[index];

    if(in_range(heap->curr_index, left_child_index))
    {
        swappable = gt_wrapped(heap->heap[left_child_index].order, min_order_node.order);
        if(swappable) min_order_node = heap->heap[left_child_index];
    }

    if(in_range(heap->curr_index, right_child_index))
    {
        swappable_right = gt_wrapped(heap->heap[right_child_index].order, min_order_node.order);
        if(swappable_right) min_order_node = heap->heap[right_child_index];
    }

    if(swappable_right)
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = min_order_node;
        heap->heap[right_child_index] = tmp;

        set_item_index(heap, index);
        set_item_index(heap, right_child_index);

        return max_heap_percolate_down(heap, right_child_index);
    }

    else if(swappable)
    {
        heap_node_t tmp = heap->heap[index];
        heap->heap[index] = min_order_node;
        heap->heap[left_child_index] = tmp;

        set_item_index(heap, index);
        set_item_index(heap, left_child_index);

        return max_heap_percolate_down(heap, left_child_index);
    }


    else return;
}


heap_op_e max_heap_insert_node(heap_t* heap, void* item, uint32_t order, heap_type_e type)
{

    if (heap->curr_index < MAX_THREADS)
    {
        heap_node_t node;
        node.item = item;
        node.order = order;
        node.type = type;

        heap->heap[heap->curr_index] = node;
        set_item_index(heap, heap->curr_index);

        max_heap_percolate_up(heap, heap->curr_index++);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e max_heap_remove_head(heap_t* heap)
{
    if (heap->curr_index == 1)
    {
        heap->curr_index--;
        return OP_OK;
    }

    else if (heap->curr_index > 1)
    {
        heap->heap[0] = heap->heap[--heap->curr_index];
        set_item_index(heap, 0);

        max_heap_percolate_down(heap, 0);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e max_heap_remove_node(heap_t* heap, size_t index)
{
    if (index >= heap->curr_index) return OP_FAILED;

    else if (heap->curr_index == 1)
    {
        heap->curr_index--;
        return OP_OK;
    }

    else if (heap->curr_index > 1)
    {
        heap->heap[index] = heap->heap[--heap->curr_index];
        set_item_index(heap, index);

        size_t parent_index = find_parent_index(index);
        if (heap->heap[index].order > heap->heap[parent_index].order) max_heap_percolate_up(heap, index);
        else max_heap_percolate_down(heap, index);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e max_heap_balance_node(heap_t* heap, size_t index, uint32_t order)
{
    if (heap->curr_index == 1) return OP_OK;

    else if (heap->curr_index > 1)
    {
        size_t parent_index = find_parent_index(index);
        heap->heap[index].order = order;
        if (heap->heap[index].order > heap->heap[parent_index].order) max_heap_percolate_up(heap, index);
        else max_heap_percolate_down(heap, index);
        return OP_OK;
    }

    else return OP_FAILED;
}


heap_op_e max_heap_pop_heap(heap_t* heap, void** item, heap_type_e type)
{
    (void)type;

    if (heap->curr_index > 0)
    {
        heap_node_t node = heap->heap[0];
        if(max_heap_remove_head(heap) == OP_FAILED) return OP_FAILED;

        *item = node.item;
        return OP_OK;
    }

    else
    {
        *item = NULL;
        return OP_FAILED;
    }
}
