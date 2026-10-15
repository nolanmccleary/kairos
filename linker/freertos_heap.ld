/* FreeRTOS comparison binary ONLY. Separate from de1-soc.ld on purpose: the Makefile
 * adds this via -T only when BENCH_FREERTOS_ALLOC=1, so the default build and
 * de1-soc.ld carry no FreeRTOS references. Augments the base script with INSERT. */

/* heap_4 + shim stay in OCRAM (part of the preloader). The FreeRTOS binary's appcode
 * switch gc-sections away the Kairos workloads, which frees enough OCRAM to fit them,
 * so no SDRAM code placement is needed and `make boot FREERTOS=1` packages normally. */

/* FreeRTOS heap_4 region (ucHeap). SDRAM is identity-mapped from 0x0; 0x01300000 is
 * cacheable SDRAM just above the 1MB host_shared Device block, i.e. the same memory
 * class as the TLSF heap so the allocator comparison is fair. NOLOAD: heap_4 fills it
 * at runtime. */
SECTIONS
{
    .freertos_heap 0x01300000 (NOLOAD) :
    {
        . = ALIGN(8);
        _freertos_heap_start = .;
        KEEP(*(.freertos_heap))
        . = ALIGN(8);
        _freertos_heap_end = .;
    }
} INSERT AFTER .host_shared;
