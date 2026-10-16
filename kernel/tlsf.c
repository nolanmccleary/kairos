/* TLSF (Tender Loving Segmentation Fault) Allocator */


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"
#include "tlsf.h"
#include "sched.h"


mutex_t g_allocator_mutex;


void *memset(void *s, int c, size_t n)   // freestanding build has no libc; sequencer.c and struct inits need it
{
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}


// Our heap is 16MB so 2^24 -> bins 0 through 23; 32 sub-bins, last bin interval (binterval haha) is [2^23, 2^24)
#define FL_COUNT 24
#define SL_COUNT 32
#define LIN 7

#define MIN_PAYLOAD 8     //Bytes needed to store prev and next pointers during free-space representation


#define INITIAL_FREE (((1<<24) - 8) & ~(0b111)) //Heap size minus the cost of one taken block header
#define TLSF_SIZE ((FL_COUNT - (LIN - 1)) * SL_COUNT) //Linear zone until 2^7 hit


extern char _heap_start;
extern char _heap_end;


#define ALIGN8(A) ((A + 0x7) & ~(0b111)) //LDRD/STRD on A9 require 8-byte alignment

#define HEAP_START (ALIGN8(((uintptr_t)&_heap_start)))
#define HEAP_END   (((uintptr_t)&_heap_end) & ~0b11)
#define HEAP_SIZE  (HEAP_END - HEAP_START)


volatile bool g_heap_initialized;


typedef struct
{
    uint32_t size : 24;
    uint32_t is_type_free_block : 1;
    uint32_t _free : 7;

}   tlsf_size_u;


typedef struct free_block // 16 Byte struct, we can't allocate less than this amount otherwise we don't have enough space to store free block representation
{
    void* prev_phys;
    tlsf_size_u size;
    struct free_block* prev;
    struct free_block* next;
}   free_block_t;


typedef struct
{
    void* prev_phys;
    tlsf_size_u size;
}   taken_block_t;


static free_block_t* tlsf_array[TLSF_SIZE]; //TODO: Adjust for lin zone optimization
static uint32_t fBitmap;
static uint32_t sBitmap[FL_COUNT - (LIN - 1)];


//IMPORTANT: Undefined behaviour for n=0
static inline uint32_t leading_one(uint32_t n)
{
    return 31 - (uint32_t)__builtin_clz(n);
}


//Given a size, this function returns the appropriate bin and sub_bin mappings
static inline allocator_op_e map_up(size_t size, uint32_t* bin_idx, uint32_t* sub_idx)
{
    if (size <= MIN_PAYLOAD) // [8,12) = tlsf[2]
    {
        *bin_idx = 6;
        *sub_idx = 2;
        return ALLOC_OP_OK;
    }

    uint32_t leader = leading_one(size);
    if (leader >= FL_COUNT)                  //Early terminate because I'm not sure GCC will infer
    {
        (void)bin_idx;
        (void)sub_idx;
        return ALLOC_OP_FAIL;
    }

    else if (leader < LIN)
    {
        leader = LIN-1;
    }

    uint32_t lead_value = 1U << leader;
    uint32_t excess = (leader == LIN-1) ? size : (size & ~(lead_value)); // linear zone uses the whole size

    uint32_t sb_size;

    if (leader == LIN-1) sb_size = 4; //                        2^(LIN=7) / (SL_COUNT=32)
    else  sb_size = lead_value >> 5; //     (Given SL_COUNT=32)


    uint32_t sb = excess / sb_size;
    uint32_t r = excess % sb_size;

    if (r != 0)
    {
        sb++;
        if(sb >= SL_COUNT)
        {
            sb = 0;
            leader++;
        }
    }

    if (leader >= FL_COUNT)
    {
        (void)bin_idx;
        (void)sub_idx;
        return ALLOC_OP_FAIL;
    }

    else
    {
        *bin_idx = leader;
        *sub_idx = sb;
        return ALLOC_OP_OK;
    }
}


static inline free_block_t* get_free(size_t size)
{
    if (size == 0) return NULL;

    uint32_t idx, sub_idx;

    if (map_up(size, &idx, &sub_idx) == ALLOC_OP_OK)
    {
        uint32_t fShift = idx;
        uint32_t fCopy = fBitmap >> fShift;
        bool tick = true;

        while (fCopy > 0)
        {
            if(fCopy & 1U) //Bin has a free sub-bin
            {
                uint32_t sShift = tick? sub_idx : 0;
                uint32_t sCopy = sBitmap[fShift - (LIN - 1)] >> sShift;

                while (sCopy > 0)
                {
                    if (sCopy & 1U) return tlsf_array[((fShift - (LIN - 1)) * SL_COUNT) + sShift];
                    sCopy >>= 1;
                    sShift++;
                }
            }

            if (tick) tick = false;

            fCopy >>= 1;
            fShift++;
        }
    }

    return NULL;
}


static inline size_t map_down(size_t size, uint32_t* fBit, uint32_t* sBit, uint32_t* sBitInd) //Returns a TLSF array index given a free block size and sets bitmaps
{
    uint32_t leader = leading_one(size);

    if (leader < LIN)
    {
        leader = LIN-1;
    }


    uint32_t lead_value = 1U << leader;
    uint32_t excess = (leader == LIN-1) ? size : (size & ~(lead_value)); // linear zone uses the whole size

    uint32_t sb_size;

    if (leader == LIN-1) sb_size = 4; //                        2^(LIN=7) / (SL_COUNT=32)
    else  sb_size = lead_value >> 5; //     (Given SL_COUNT=32)


    uint32_t sb = excess / sb_size;


    *fBit = lead_value;
    *sBit = 1 << sb;
    *sBitInd = leader - (LIN - 1);

    return (leader - (LIN - 1)) * SL_COUNT + sb;
}


void* kMalloc(size_t size)
{
    size = ALIGN8(size);


    free_block_t* free_block = get_free(size);   // size==0 -> NULL; sub-min handled by map_up's check chain
    if (free_block != NULL)
    {
        size_t orig_size = free_block->size.size;

        uint32_t ofBit, osBit, osBitInd;                                 // free_block's own bin, for removal below
        uint32_t obin = map_down(orig_size, &ofBit, &osBit, &osBitInd);

        size_t new_size = orig_size - size - sizeof(taken_block_t);

        if (orig_size >= size + sizeof(taken_block_t) + MIN_PAYLOAD) // If we have enough for another block we split
        {
            free_block_t* new_block = (free_block_t*)((char*)free_block + size + sizeof(taken_block_t));
            new_block->size.size = new_size;
            new_block->size.is_type_free_block = 1;
            new_block->prev_phys = free_block;

            free_block_t* after = (free_block_t*)((char*)new_block + new_size + sizeof(taken_block_t));
            if ((uintptr_t)after < HEAP_END) after->prev_phys = new_block; // remainder is now the after-block's prev


            free_block->size.size = size;


            uint32_t fBit, sBit, sBitInd;
            uint32_t tlsf_idx = map_down(new_size, &fBit, &sBit, &sBitInd);

            fBitmap |= fBit;
            sBitmap[sBitInd] |= sBit;


            free_block_t* head = tlsf_array[tlsf_idx];

            new_block->prev = NULL;
            new_block->next = head;
            if (head != NULL) head->prev = new_block;

            tlsf_array[tlsf_idx] = new_block;
        }


        free_block->size.is_type_free_block = 0;

        free_block_t* prev = free_block->prev;
        free_block_t* next = free_block->next;

        if (prev != NULL)
        {
            prev->next = next;
        }

        else
        {
            tlsf_array[obin] = next;    // free_block was the list head
        }

        if (next != NULL)
        {
            next->prev = prev;
        }

        if (tlsf_array[obin] == NULL)
        {
            sBitmap[osBitInd] &= ~osBit;
            if (sBitmap[osBitInd] == 0) fBitmap &= ~ofBit;
        }


        return (void*)((char*)free_block + sizeof(taken_block_t));
    }

    return NULL;
}


allocator_op_e kFree(void* target)
{
    if (target == NULL) return ALLOC_OP_FAIL;

    taken_block_t* curr = (taken_block_t*)((char*)target - sizeof(taken_block_t));

    if (curr->size.is_type_free_block) return ALLOC_OP_FAIL;

    size_t init_size = curr->size.size;
    size_t new_free_size = init_size;


    free_block_t* prev_phys = (free_block_t*)curr->prev_phys;

    while (prev_phys != NULL && prev_phys->size.is_type_free_block)
    {
        uint32_t prevSize = prev_phys->size.size;
        new_free_size += prevSize + sizeof(taken_block_t);

        free_block_t* prev = prev_phys->prev;
        free_block_t* next = prev_phys->next;

        uint32_t fBit, sBit, sBitInd;
        uint32_t tlsf_idx = map_down(prevSize, &fBit, &sBit, &sBitInd);

        if (prev == NULL && next == NULL)
        {
            tlsf_array[tlsf_idx] = NULL;
            sBitmap[sBitInd] &= ~sBit;
            if (sBitmap[sBitInd] == 0) fBitmap &= ~fBit;   // only clear octave bit once its last sub-bin empties
        }

        else
        {
            if (prev != NULL)
            {
                prev->next = next;
            }

            else
            {
                tlsf_array[tlsf_idx] = next;    // prev_phys was the list head
            }

            if (next != NULL)
            {
                next->prev = prev;
            }
        }


        curr = (taken_block_t*)prev_phys;
        prev_phys = (free_block_t*)curr->prev_phys;
    }


    free_block_t* next_phys = (free_block_t*)((char*)target + init_size);

    while ((uintptr_t)next_phys < HEAP_END && next_phys->size.is_type_free_block)
    {
        uint32_t nextSize = next_phys->size.size;
        new_free_size += nextSize + sizeof(taken_block_t);

        free_block_t* prev = next_phys->prev;
        free_block_t* next = next_phys->next;

        uint32_t fBit, sBit, sBitInd;
        uint32_t tlsf_idx = map_down(nextSize, &fBit, &sBit, &sBitInd);

        if (prev == NULL && next == NULL)
        {
            tlsf_array[tlsf_idx] = NULL;
            sBitmap[sBitInd] &= ~sBit;
            if (sBitmap[sBitInd] == 0) fBitmap &= ~fBit;   // only clear octave bit once its last sub-bin empties
        }

        else
        {
            if (prev != NULL)
            {
                prev->next = next;
            }

            else
            {
                tlsf_array[tlsf_idx] = next;    // next_phys was the list head
            }

            if (next != NULL)
            {
                next->prev = prev;
            }
        }

        next_phys = (free_block_t*)((char*)next_phys + nextSize + sizeof(taken_block_t));
    }


    free_block_t* new_free = (free_block_t*)curr;
    new_free->size.size = new_free_size;
    new_free->size.is_type_free_block = 1;

    free_block_t* after = (free_block_t*)((char*)new_free + new_free_size + sizeof(taken_block_t));
    if ((uintptr_t)after < HEAP_END) after->prev_phys = new_free; // keep the after-block's boundary tag pointing at us

    uint32_t fBit, sBit, sBitInd;
    uint32_t tlsf_idx = map_down(new_free_size, &fBit, &sBit, &sBitInd);

    fBitmap |= fBit;
    sBitmap[sBitInd] |= sBit;

    free_block_t* head = tlsf_array[tlsf_idx];

    new_free->prev = NULL;
    new_free->next = head;
    if (head != NULL) head->prev = new_free;

    tlsf_array[tlsf_idx] = new_free;

    return ALLOC_OP_OK;
}


allocator_op_e heap_init(void)
{
    if (!g_heap_initialized)
    {
        for (size_t i = 0; i < TLSF_SIZE-1; i++)
        {
            tlsf_array[i] = NULL;
        }

        for (size_t i = 0; i < FL_COUNT - (LIN - 1); i++)   // clear every sub-bitmap so a re-init leaves no stale bits
        {
            sBitmap[i] = 0;
        }

        free_block_t* sentinel = (free_block_t*)((void*)&_heap_start);
        sentinel->prev_phys = NULL;
        sentinel->prev = NULL;
        sentinel->next = NULL;
        sentinel->size.size = INITIAL_FREE;
        sentinel->size.is_type_free_block = 1U;

        tlsf_array[TLSF_SIZE-1U] = sentinel;

        fBitmap = 1U << (FL_COUNT-1U);
        sBitmap[FL_COUNT-LIN] = 1U << (SL_COUNT-1U);

        g_heap_initialized = true;

        return ALLOC_OP_OK;
    }

    return ALLOC_OP_FAIL;
}


allocator_op_e heap_destroy(void)
{
    if (g_heap_initialized)
    {

        g_cpus[CPU0].sched_init = false;
#if ENABLE_SMP == 1
        g_cpus[CPU1].sched_init = false;
#endif


        size_t i;

        for (i = 0; i < TLSF_SIZE; i++)
        {
            tlsf_array[i] = NULL;
        }

        fBitmap = 0;

        for (i = 0; i < FL_COUNT - (LIN - 1U); i++)
        {
            sBitmap[i] = 0;
        }

        g_heap_initialized = false;

        return ALLOC_OP_OK;
    }

    return ALLOC_OP_FAIL;
}


allocator_op_e heap_reset(void)
{
    if (g_heap_initialized)
    {
        heap_destroy();
        heap_init();
        return ALLOC_OP_OK;
    }

    return ALLOC_OP_FAIL;
}

