#include <stdint.h>

#include "telemetry.h"
#include "tlsf.h"


#define ALLOC_SIZE_BYTES    64
#define LOADED_ALLOC_COUNT  64
#define ALLOC_ITERS         512


/* g_metrics slots -> "malloc" / "free" / "malloc_loaded" / "free_loaded" (test.py).
 * ALLOC_OP_COUNT is the terminal count, used to size the per-iteration sample table. */
typedef enum
{
    MALLOC = 0,
    FREE,
    MALLOC_LOADED,
    FREE_LOADED,
    ALLOC_OP_COUNT
} malloc_op_e;


static void* loaded_blocks[LOADED_ALLOC_COUNT];


/* Every per-iteration cycle count, so the host can plot the full timing
 * distribution (the g_metrics slots keep only min/max/mean). SDRAM-resident
 * (8192 bytes); the host reads ALLOC_ITERS samples per op. */
SDRAM_UNCACHED_TELEMETRY uint32_t g_alloc_samples[ALLOC_OP_COUNT][ALLOC_ITERS];


void allocbench_run(void)
{
    void* block;

    metric_reset(MALLOC);
    metric_reset(FREE);
    metric_reset(MALLOC_LOADED);
    metric_reset(FREE_LOADED);


    /* ---------------------------------------------------------------------
     * Empty heap
     * ------------------------------------------------------------------------- */

    heap_init();


    for (int i = 0; i < ALLOC_ITERS; i++)
    {
        MEASURE_BEGIN(MALLOC);

        block = kMalloc(ALLOC_SIZE_BYTES);

        MEASURE_END_INTO(MALLOC, g_alloc_samples[MALLOC][i]);


        MEASURE_BEGIN(FREE);

        kFree(block);

        MEASURE_END_INTO(FREE, g_alloc_samples[FREE][i]);
    }


    heap_destroy();


    /* ---------------------------------------------------------------------
     * Loaded heap
     * ------------------------------------------------------------------------- */

    heap_init();


    for (int i = 0; i < LOADED_ALLOC_COUNT; i++)
    {
        loaded_blocks[i] = kMalloc(ALLOC_SIZE_BYTES);
    }


    for (int i = 0; i < ALLOC_ITERS; i++)
    {
        MEASURE_BEGIN(MALLOC_LOADED);

        block = kMalloc(ALLOC_SIZE_BYTES);

        MEASURE_END_INTO(MALLOC_LOADED, g_alloc_samples[MALLOC_LOADED][i]);


        MEASURE_BEGIN(FREE_LOADED);

        kFree(block);

        MEASURE_END_INTO(FREE_LOADED, g_alloc_samples[FREE_LOADED][i]);
    }


    /*
     * Restore allocator state before EDF starts.
     */
    for (int i = 0; i < LOADED_ALLOC_COUNT; i++)
    {
        kFree(loaded_blocks[i]);
    }


    heap_destroy();
}
