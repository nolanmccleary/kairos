/* Allocator alloc/free cost as a function of payload size. Allocator-agnostic:
 * the normal build measures Kairos TLSF (kMalloc/kFree); the FreeRTOS build
 * (USE_FREERTOS) measures heap_4 (pvPortMalloc/vPortFree). Same file, same sweep,
 * same host-shared symbols, so test.py and test_freertos.py read identical layouts.
 * See freertos_port/PLAN.md. */

#include <stdint.h>

#include "telemetry.h"

#if USE_FREERTOS
#include "FreeRTOS.h"
#define SWEEP_ALLOC( n )   pvPortMalloc( n )
#define SWEEP_FREE( p )    vPortFree( p )
#define SWEEP_HEAP_PREP()  ( ( void ) 0 )   /* heap_4 initialises ucHeap lazily */
#define SWEEP_HEAP_DONE()  ( ( void ) 0 )
#else
#include "tlsf.h"
#define SWEEP_ALLOC( n )   kMalloc( n )
#define SWEEP_FREE( p )    kFree( p )
#define SWEEP_HEAP_PREP()  heap_init()
#define SWEEP_HEAP_DONE()  heap_destroy()   /* leave TLSF pristine for later phases (EDF etc.) */
#endif

#define SWEEP_ITERS  256u


/* Payload sizes swept, bytes. Geometric, 16 B .. 64 KiB. */
static const uint32_t sweep_sizes[] =
{
    16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u, 65536u
};

#define SWEEP_N_SIZES  ( sizeof( sweep_sizes ) / sizeof( sweep_sizes[ 0 ] ) )


/* Host-readable: the x-axis (the sizes + their count) and, per size, the alloc and
 * free cycle distributions (min / max / sum / count -> mean). */
HOST_SHARED uint32_t g_allocsweep_sizes[ SWEEP_N_SIZES ];
HOST_SHARED uint32_t g_allocsweep_n;
HOST_SHARED metric_t g_allocsweep_alloc[ SWEEP_N_SIZES ];
HOST_SHARED metric_t g_allocsweep_free[ SWEEP_N_SIZES ];


static inline void sw_reset( metric_t* m )
{
    m->min = 0xFFFFFFFFu;
    m->max = 0u;
    m->sum = 0u;
    m->count = 0u;
}

static inline void sw_add( metric_t* m, uint32_t cycles )
{
    if( cycles < m->min ) m->min = cycles;
    if( cycles > m->max ) m->max = cycles;
    m->sum   += cycles;
    m->count += 1u;
}


void allocsweep_run( void )
{
    SWEEP_HEAP_PREP();

    g_allocsweep_n = ( uint32_t ) SWEEP_N_SIZES;

    for( uint32_t s = 0; s < SWEEP_N_SIZES; s++ )
    {
        g_allocsweep_sizes[ s ] = sweep_sizes[ s ];
        sw_reset( &g_allocsweep_alloc[ s ] );
        sw_reset( &g_allocsweep_free[ s ] );

        /* Each block is freed immediately, so the free list stays short and we are
         * measuring the cost as a function of requested size, not fragmentation. */
        for( uint32_t i = 0; i < SWEEP_ITERS; i++ )
        {
            uint32_t t0 = pmu_cycles();
            void* p = SWEEP_ALLOC( sweep_sizes[ s ] );
            uint32_t d = pmu_cycles() - t0;
            sw_add( &g_allocsweep_alloc[ s ], d );

            t0 = pmu_cycles();
            SWEEP_FREE( p );
            d = pmu_cycles() - t0;
            sw_add( &g_allocsweep_free[ s ], d );
        }
    }

    SWEEP_HEAP_DONE();
}
