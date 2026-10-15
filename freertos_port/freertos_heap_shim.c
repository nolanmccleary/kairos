/* FreeRTOS battery heap backing. configAPPLICATION_ALLOCATED_HEAP == 1, so heap_4.c
 * uses `extern uint8_t ucHeap[...]`; OCRAM is only 65536 bytes, so the heap_4 region
 * lives in SDRAM via the .freertos_heap NOLOAD section (linker/freertos_heap.ld). The
 * scheduler's suspend/resume + critical-section routines that heap_4 brackets with come
 * from tasks.c/port.c (the scheduler is always linked in this battery). */

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

__attribute__( ( section( ".freertos_heap" ), aligned( 8 ) ) )
uint8_t ucHeap[ configTOTAL_HEAP_SIZE ];
