#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"
#include "boot.h"
#include "bsp.h"
#include "sequencer.h"
#include "gic.h"
#include "timers.h"
#include "tlsf.h"
#include "preempt_sched.h"
#include "pmu.h"
#include "sd.h"

#if ENABLE_DCACHE && !ENABLE_MMU
#error ENABLE_DCACHE requires ENABLE_MMU
#endif

#if ENABLE_L2 && !ENABLE_MMU
#error ENABLE_L2 requires ENABLE_MMU (L2 only caches MMU-marked outer-cacheable memory)
#endif

#if ENABLE_SMP && !ENABLE_MMU
#error ENABLE_SMP requires ENABLE_MMU (SCU coherency is over cacheable memory)
#endif




//TODO: Null guarded zone is first section (1Mb) right now. This is excessive and should be moved to 
// first page at some point





#if ENABLE_SMP
volatile uint32_t g_cpu1_ready;


static void scu_init(void)
{
    SCU_INVALIDATE_ALL = SCU_INVALIDATE_ALL_WAYS;
    SCU_CTRL |= SCU_CTRL_ENABLE;
    __asm__ volatile ("dsb" ::: "memory");
}
#endif


///////////////////////////////////////////// L2 (PL310) INIT ///////////////////////

#if ENABLE_L2

static void l2_cache_init(void)
{
    if (PL310_CTRL & PL310_CTRL_ENABLE)
    {
        PL310_CLEAN_INV_WAY = PL310_ALL_WAYS;
        while (PL310_CLEAN_INV_WAY & PL310_ALL_WAYS) { }
        PL310_CTRL = 0u;
    }

    PL310_TAG_LATENCY  = PL310_TAG_LATENCY_VAL;    /* tag  1/1/1 cycles  */
    PL310_DATA_LATENCY = PL310_DATA_LATENCY_VAL;   /* data 2/1/1 cycles  */

    /* Keep the reset aux value's RTL-fixed geometry (8-way, 64 KB/way) and add:
       shared-override (so our SHAREABLE SDRAM is cached in L2 at all -- the same trap
       as ACTLR.SMP for L1), round-robin replacement, and data + instruction prefetch. */
    PL310_AUX_CTRL |= PL310_AUX_SHARED_OVERRIDE
                    | PL310_AUX_REPLACE_ROUNDROBIN
                    | PL310_AUX_DATA_PREFETCH
                    | PL310_AUX_INSTR_PREFETCH;

    /* L2 RAM is garbage at power-on: invalidate all ways, wait for completion, enable. */
    PL310_INV_WAY = PL310_ALL_WAYS;
    while (PL310_INV_WAY & PL310_ALL_WAYS) { }

    PL310_CTRL = PL310_CTRL_ENABLE;
    __asm__ volatile ("dsb" ::: "memory");
}
#endif


///////////////////////////////////////////// MMU INIT /////////////////////////////






extern uint32_t _l1_table_base[];   /* reserved 16384-byte L1 table in DDR (16 KB-aligned) */
extern uint32_t _l2_table_base[];   /* reserved 1024-byte  L2 table in DDR (1 KB-aligned)  */
extern char     _sdram_length[];    /* = LENGTH(SDRAM): total DDR size in bytes */
extern char     _ocram_origin[];    /* = ORIGIN(OCRAM): OCRAM base address      */



#define L1_BLOCK_ADDR_SHIFT       20u                                  /* block base addr = block_index << 20 */
#define L1_BLOCK_SIZE_BYTES       (1u << L1_BLOCK_ADDR_SHIFT)          /* 1048576 bytes per L1 block */
#define L1_BLOCK_COUNT            (1u << (32u - L1_BLOCK_ADDR_SHIFT))  /* 4096 blocks cover the whole 4.29e9-byte space */


#define L2_BLOCK_ADDR_SHIFT       12u                                        /* sub-block base = index << 12 */
#define L2_BLOCK_SIZE_BYTES       (1u << L2_BLOCK_ADDR_SHIFT)                /* 4096 bytes per L2 sub-block */
#define L2_BLOCK_COUNT            (L1_BLOCK_SIZE_BYTES / L2_BLOCK_SIZE_BYTES)/* 1048576 / 4096 = 256 sub-blocks per L1 block */



#define L1_ENTRY_MAP_BLOCK_CACHEABLE  0x15DE6u  /* kind 0b10: maps its 1048576-byte block directly; Normal WBWA shareable */
#define L1_ENTRY_MAP_BLOCK_DEVICE     0x0DE2u   /* kind 0b10: maps its block directly; Strongly-ordered (uncached) */
#define L1_ENTRY_POINT_TO_L2          0x001E1u  /* kind 0b01: no memory type; delegates the block to an L2 table (domain 15) */

/* Complete L2 sub-block entries except the address field -- OR the sub-block base on. */
#define L2_ENTRY_MAP_SUBBLOCK_CACHEABLE  0x00576u  /* Normal WBWA shareable, small sub-block layout */
#define L2_ENTRY_MAP_SUBBLOCK_DEVICE     0x00032u  /* Strongly-ordered,       small sub-block layout */

#define SCTLR_ENABLE_MMU          0x0001u
#define SCTLR_ENABLE_DCACHE       0x0004u
#define SCTLR_ENABLE_ICACHE       0x1000u
#define ACTLR_ENABLE_SMP          0x0040u        /* coherency; must be set before caches on the A9 */
#define DACR_ALL_DOMAINS_CLIENT   0x55555555u





///////////////////////////////////////////// STARTUP /////////////////////////////






static void bsp_sdram_init(void)
{
    // RSTMGR_PERMODRST |= RSTMGR_PERMODRST_L4WD0 | RSTMGR_PERMODRST_L4WD1;

#ifdef BOARD_DE1_SOC
    pll_init();
    scan_mgr_init();         //PHY ANA
    sdram_ctrl_init();
    (void)sdram_calibration_full((struct socfpga_sdr *)0xFFC20000U);
    PL310_FILTER_END   = 0x40000000U;  /* SDRAM window: 0x0..0x3FFFFFFF -> M1 */
    PL310_FILTER_START = 0x00000001U;  /* enable filter, start = 0x0 */
    NIC301_REMAP       = 0;           // Clear remap because system can see sdram exists now
#endif
}


static void bsp_precache_init(void)
{
#if ENABLE_SMP
    scu_init();        /* enable SCU coherency before caches/ACTLR.SMP come on */
#endif

#if ENABLE_L2
    l2_cache_init();   /* enable the outer (L2) cache before the MMU + L1 come on */
#endif

    (void)(0);
}



static void build_tables(void)
{
#if ENABLE_MMU
    volatile uint32_t *l1_table = _l1_table_base;

#if BOARD == BOARD_QEMU
    /* Flat identity map. QEMU has one DRAM window (_ocram_origin = 0x60000000, size
       _sdram_length): map it Normal cacheable + shareable so ACTLR.SMP / SCU snoop
       is consistent, every other 1MB block (GIC/timer/peripherals) Device. No OCRAM
       delegation -- QEMU has no separate OCRAM, and TCG RAM is coherent regardless. */
    uint32_t ram_first = (uint32_t)(uintptr_t)_ocram_origin >> L1_BLOCK_ADDR_SHIFT;
    uint32_t ram_last  = ram_first + ((uint32_t)(uintptr_t)_sdram_length >> L1_BLOCK_ADDR_SHIFT);
    for (uint32_t b = 0; b < L1_BLOCK_COUNT; b++)
    {
        uint32_t base  = b << L1_BLOCK_ADDR_SHIFT;
        uint32_t entry = (b >= ram_first && b < ram_last) ? L1_ENTRY_MAP_BLOCK_CACHEABLE
                                                          : L1_ENTRY_MAP_BLOCK_DEVICE;
        l1_table[b] = base | entry;
    }
#else
    uint32_t sdram_block_count = (uint32_t)(uintptr_t)_sdram_length / L1_BLOCK_SIZE_BYTES;

    /* The bulk telemetry block lives in SDRAM but must be Device (JTAG-coherent),
       not cacheable like the rest of DDR. Its linker section is one 1MB block. */
    extern char _host_shared_sdram_start[];
    extern char _host_shared_sdram_end[];
    uint32_t sdram_hs_start = (uint32_t)(uintptr_t)_host_shared_sdram_start;
    uint32_t sdram_hs_end   = (uint32_t)(uintptr_t)_host_shared_sdram_end;

    uint32_t delegated_block = (uint32_t)(uintptr_t)_ocram_origin >> L1_BLOCK_ADDR_SHIFT;

    /* Map each block directly: SDRAM cacheable, the SDRAM telemetry block Device,
       every block beyond DDR Device. */
    for (uint32_t l1_block = 0; l1_block < L1_BLOCK_COUNT; l1_block++)
    {

        if (l1_block == delegated_block)
            continue;                 /* handled by the L2 delegation below, not here */

        uint32_t l1_block_base  = l1_block << L1_BLOCK_ADDR_SHIFT;
        bool     in_sdram       = (l1_block < sdram_block_count);
        bool     in_host_shared = (l1_block_base >= sdram_hs_start) && (l1_block_base < sdram_hs_end);
        uint32_t l1_entry       = (in_sdram && !in_host_shared) ? L1_ENTRY_MAP_BLOCK_CACHEABLE
                                                                : L1_ENTRY_MAP_BLOCK_DEVICE;
        l1_table[l1_block] = l1_block_base | l1_entry;
    }

#if ENABLE_NULL_GUARD
    l1_table[0] = 0u;
#endif

    /* Fill the L2 table for the delegated block (0xFFF00000..0xFFFFFFFF, which holds
       the boot ROM, GIC/timer, and OCRAM). Rule: ALL of OCRAM (>= _ocram_origin) is
       Normal-cacheable -- text/data/bss/stacks -- EXCEPT the host_ocram island, which
       stays Device so JTAG reads/writes it coherently. Everything below OCRAM in the
       block (boot ROM, GIC, timer) is Device. This L2 table is the SOLE source of
       memory type for the block. */
    extern char _host_shared_ocram_start[];
    extern char _host_shared_ocram_end[];
    uint32_t ocram_start = (uint32_t)(uintptr_t)_ocram_origin;
    uint32_t hs_start    = (uint32_t)(uintptr_t)_host_shared_ocram_start;
    uint32_t hs_end      = (uint32_t)(uintptr_t)_host_shared_ocram_end;

    volatile uint32_t *l2_table = _l2_table_base;

    for (uint32_t l2_block = 0; l2_block < L2_BLOCK_COUNT; l2_block++)
    {
        uint32_t l2_block_base = (delegated_block << L1_BLOCK_ADDR_SHIFT)
                               + (l2_block << L2_BLOCK_ADDR_SHIFT);
        bool      in_ocram       = (l2_block_base >= ocram_start);
        bool      in_host_shared = (l2_block_base >= hs_start) && (l2_block_base < hs_end);
        uint32_t l2_entry       = (in_ocram && !in_host_shared) ? L2_ENTRY_MAP_SUBBLOCK_CACHEABLE
                                                                : L2_ENTRY_MAP_SUBBLOCK_DEVICE;

        l2_table[l2_block] = l2_block_base | l2_entry;
    }

    l1_table[delegated_block] = (uint32_t)(uintptr_t)_l2_table_base | L1_ENTRY_POINT_TO_L2;
#endif  /* BOARD == BOARD_QEMU / else */
#endif  /* ENABLE_MMU */

    (void)(0);
}



static void set_ttb_ptrs(void)
{
#if ENABLE_MMU
    uint32_t ttbr0          = (uint32_t)(uintptr_t)_l1_table_base;
    uint32_t dacr           = DACR_ALL_DOMAINS_CLIENT;
    uint32_t smp_bit        = ACTLR_ENABLE_SMP;
    uint32_t sctlr_enables  = SCTLR_ENABLE_MMU;
#if ENABLE_DCACHE
    sctlr_enables |= SCTLR_ENABLE_DCACHE;
#endif
#if ENABLE_ICACHE
    sctlr_enables |= SCTLR_ENABLE_ICACHE;
#endif

    uint32_t scratch;
    __asm__ volatile (
        "mrc     p15, 0, %0, c1, c0, 1\n\t"   /* read  ACTLR                         */
        "orr     %0, %0, %4\n\t"              /* set   SMP (coherency, before caches)*/
        "mcr     p15, 0, %0, c1, c0, 1\n\t"   /* write ACTLR                         */
        "isb\n\t"
        "mov     %0, #0\n\t"
        "mcr     p15, 0, %0, c2, c0, 2\n\t"   /* TTBCR = 0  (all translation via TTBR0) */
        "mcr     p15, 0, %1, c2, c0, 0\n\t"   /* TTBR0 = L1 table base               */
        "mcr     p15, 0, %2, c3, c0, 0\n\t"   /* DACR                                */
        "dsb\n\t"
        "mrc     p15, 0, %0, c1, c0, 0\n\t"   /* read  SCTLR                         */
        "orr     %0, %0, %3\n\t"              /* set   MMU / cache enable bits       */
        "mcr     p15, 0, %0, c1, c0, 0\n\t"   /* write SCTLR  (MMU + caches on)      */
        "isb\n"
        : "=&r"(scratch)
        : "r"(ttbr0), "r"(dacr), "r"(sctlr_enables), "r"(smp_bit)
        : "memory"
    );
#elif ENABLE_ICACHE
    uint32_t icache_bit = SCTLR_ENABLE_ICACHE;
    uint32_t scratch;
    __asm__ volatile (
        "mrc     p15, 0, %0, c1, c0, 0\n\t"   /* read  SCTLR                */
        "orr     %0, %0, %1\n\t"              /* set   I-cache (no MMU needed) */
        "mcr     p15, 0, %0, c1, c0, 0\n\t"   /* write SCTLR                */
        "isb\n"
        : "=&r"(scratch)
        : "r"(icache_bit)
        : "memory"
    );
#endif

    (void)(0);
}




void cpu0_startup(void)
{
#if ENABLE_SMP
    while (!g_cpu1_ready){}    /* wait for CPU1 in its spin loop before the remap below */
#endif

    bsp_sdram_init();               /* PLL/scan-mgr/SDRAM/NIC301 -- prime self-boot suspect */
    bsp_precache_init();
    build_tables();
    set_ttb_ptrs();
    cpu0_gic_init();
    cpu0_timer_start();

    pmu_init();
    heap_init();        // must precede psched_init(): it kMalloc's main_thread + the deque

    load_from_sd();


#if ENABLE_SMP
    g_cpu_mailbox_uncached = 1;
    __asm__ volatile (
        "dsb\n\t"
        "sev\n\t"
    );

    while (g_cpu_mailbox_uncached != 2)
    {
        __asm__ volatile (
            "wfe\n\t"
        );
    }

    g_cpu_mailbox_uncached = 0;
#endif


    __asm__ volatile (
        "cpsie i\n\t"
    );

    psched_init();
}




void cpu1_startup(void)
{
    set_ttb_ptrs();
    cpu1_gic_init();
    cpu1_timer_start();
    pmu_init();
}
