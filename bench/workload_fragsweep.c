/* Allocator alloc/free cost as a function of FREE-LIST LENGTH (fragmentation). This
 * is the comparison that matters for an RTOS: TLSF is O(1) regardless, heap_4's
 * first-fit degrades to O(n) as the free list grows. Allocator-agnostic via include
 * guards, same as workload_allocsweep.c. See freertos_port/PLAN.md. */

#include <stdint.h>

#include "telemetry.h"

#if USE_FREERTOS
#include "FreeRTOS.h"
#define FS_ALLOC( n )   pvPortMalloc( n )
#define FS_FREE( p )    vPortFree( p )
#define FS_PREP()       ( ( void ) 0 )
#define FS_DONE()       ( ( void ) 0 )
#else
#include "tlsf.h"
#define FS_ALLOC( n )   kMalloc( n )
#define FS_FREE( p )    kFree( p )
#define FS_PREP()       heap_init()
#define FS_DONE()       heap_destroy()
#endif

#define FS_HOLE_SIZE   32u    /* hole blocks; big enough to thread a next-pointer through */
#define FS_ALLOC_SIZE  64u    /* > hole, so first-fit must skip EVERY hole to serve it */
#define FS_ITERS       64u


/* Free-list lengths (number of holes) swept. */
static const uint32_t frag_levels[] =
{
    1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u
};

#define FS_N_LEVELS  ( sizeof( frag_levels ) / sizeof( frag_levels[ 0 ] ) )


/* Host-readable: the x-axis (free-list length) and, per level, the alloc and free
 * cost distributions. Same metric_t layout as the size sweep. */
HOST_SHARED uint32_t g_fragsweep_levels[ FS_N_LEVELS ];
HOST_SHARED uint32_t g_fragsweep_n;
HOST_SHARED metric_t g_fragsweep_alloc[ FS_N_LEVELS ];
HOST_SHARED metric_t g_fragsweep_free[ FS_N_LEVELS ];


static inline void fs_reset( metric_t* m )
{
    m->min = 0xFFFFFFFFu;
    m->max = 0u;
    m->sum = 0u;
    m->count = 0u;
}

static inline void fs_add( metric_t* m, uint32_t cycles )
{
    if( cycles < m->min ) m->min = cycles;
    if( cycles > m->max ) m->max = cycles;
    m->sum   += cycles;
    m->count += 1u;
}


void fragsweep_run( void )
{
    FS_PREP();

    g_fragsweep_n = ( uint32_t ) FS_N_LEVELS;

    for( uint32_t li = 0; li < FS_N_LEVELS; li++ )
    {
        uint32_t L = frag_levels[ li ];
        g_fragsweep_levels[ li ] = L;
        fs_reset( &g_fragsweep_alloc[ li ] );
        fs_reset( &g_fragsweep_free[ li ] );

        /* Allocate 2L contiguous HOLE_SIZE blocks and thread them onto a LIFO list
         * stored IN the blocks' own payload (no external array, so no OCRAM cost). */
        void* chain = 0;
        for( uint32_t i = 0; i < 2u * L; i++ )
        {
            void* p = FS_ALLOC( FS_HOLE_SIZE );
            *( void** ) p = chain;
            chain = p;
        }

        /* Free every other block, keeping the rest. The kept blocks separate the freed
         * ones so the free list holds ~L non-coalescable entries. Re-thread the kept
         * (held) blocks onto their own list for cleanup. */
        void* held = 0;
        uint32_t k = 0;
        for( void* cur = chain; cur != 0; )
        {
            void* next = *( void** ) cur;
            if( ( k & 1u ) != 0u )
            {
                FS_FREE( cur );
            }
            else
            {
                *( void** ) cur = held;
                held = cur;
            }
            k++;
            cur = next;
        }

        /* Measure an alloc too large for the holes (first-fit must walk all L of them)
         * and its free, at this fragmentation level. */
        for( uint32_t i = 0; i < FS_ITERS; i++ )
        {
            uint32_t t0 = pmu_cycles();
            void* p = FS_ALLOC( FS_ALLOC_SIZE );
            uint32_t d = pmu_cycles() - t0;
            fs_add( &g_fragsweep_alloc[ li ], d );

            t0 = pmu_cycles();
            FS_FREE( p );
            d = pmu_cycles() - t0;
            fs_add( &g_fragsweep_free[ li ], d );
        }

        /* Free the held blocks -> the whole heap coalesces back to pristine. */
        for( void* cur = held; cur != 0; )
        {
            void* next = *( void** ) cur;
            FS_FREE( cur );
            cur = next;
        }
    }

    FS_DONE();
}
