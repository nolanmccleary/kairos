OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_reset_handler)


_sdram_length = 0x40000000;   /* 1073741824 bytes (1.07e9) of HPS DDR3 */


MEMORY
{
    SDRAM (rwx) : ORIGIN = 0x00000000, LENGTH = _sdram_length
    OCRAM (rwx) : ORIGIN = 0xFFFF0000, LENGTH = 64K
}


_ocram_origin = ORIGIN(OCRAM);


/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

_stack_size     = 0x300;   /* temporary: freed OCRAM to fit the micro workload; revert once kernel/app text split moves app code to SDRAM */
_abt_stack_size = 0x200;
_und_stack_size = 0x200;

_bank_size = 0x80;

_appcode_offset = 0x00100000;   /* 1 MB: free SDRAM below the heap, above the mmu tables */

_heap_offset = 0x200000;
_heap_size   = 0x01000000;

/* host_shared telemetry sits in its own 1MB SDRAM block directly above the heap
 * (0x1200000). build_tables() maps that block Device so JTAG phys reads are
 * coherent with CPU writes. */
_host_shared_offset = _heap_offset + _heap_size;


_preloader_header_offset = 0x40;
_preloader_header_size   = 0x0C;


_l3_boot_region_end = 0x00100000;   /* 1048576 bytes */


SECTIONS
{

  /* 4096 l1 sections at 1MB each; 256 l2 pages at 2^12 bytes each to cover the unmapped zone 
   * */ 
    
    .mmu_tables ORIGIN(SDRAM) (NOLOAD) :
    {
        . = ALIGN(16K);
        _l1_table_base = .;
        . = . + 4096 * 4;      /* 4096 L1 entries x 4 bytes = 16384 bytes */

        . = ALIGN(1K);
        _l2_table_base = .;
        . = . + 256 * 4;       /* 256 L2 entries x 4 bytes = 1024 bytes */

        _mmu_tables_end = .;
    } > SDRAM



    /* ---------------------------------------------------------------------
     * SDRAM application/test code, copied in from the SD card by load_from_sd()
     * after SDRAM is up (see bsp/sd.c). NOLOAD: it is NOT in preloader.bin -- the
     * boot ROM only loads the OCRAM kernel; this region arrives from app.bin on SD.
     * Empty until bench/*.o is relocated here (and flash_sd.sh writes app.bin).
     * --------------------------------------------------------------------- */

    .appcode ORIGIN(SDRAM) + _appcode_offset :
    {
        . = ALIGN(8);
        _appcode_start = .;
        *workload_*.o(.text*)   /* workload CODE only. .rodata stays in OCRAM: the host reads the
                                 * const config (g_edf_u_count/periods/u_values) at the boot gate,
                                 * before cpu0_startup brings SDRAM up. NOT ktrace/telemetry. */
        . = ALIGN(8);
        _appcode_end = .;
    } > SDRAM



    /* ---------------------------------------------------------------------
     * SDRAM heap
     * --------------------------------------------------------------------- */

    .heap ORIGIN(SDRAM) + _heap_offset (NOLOAD) :
    {
        . = ALIGN(8);

        _heap_start = .;

        . = . + _heap_size;

        _heap_end = .;
    } > SDRAM




    /* Bulk host-readable telemetry -- its own 1MB Device-mapped SDRAM block,
     * directly above the heap. NOLOAD: firmware writes it at runtime. */
    .host_shared ORIGIN(SDRAM) + _host_shared_offset (NOLOAD) :
    {
        _host_shared_sdram_start = .;

        KEEP(*(.telemetry))

        . = ALIGN(8);

        _host_shared_sdram_end = .;
    } > SDRAM




    .itext ORIGIN(OCRAM) :
    {
        _itext_start = .;
        KEEP(*(._vectors))   /* 8-entry branch table expected, 0x00..0x1F */
        . = _itext_start + _preloader_header_offset;
        _preloader_header = .;
        . = . + _preloader_header_size;
        KEEP(*(.boot_entry))


        *(.text*)
        *(.rodata*)

        . = ALIGN(0x1000);
        _itext_end = .;
    } > OCRAM





    /* OCRAM host island: only what must be reachable when SDRAM is not --
     * crash records (.fault), the pre-c_startup boot gate, and the SMP mailbox
     * (.host_ocram). Device-mapped by the L2 loop in build_tables(). */
    .host_ocram ALIGN(0x1000) (NOLOAD) :
    {
        _host_shared_ocram_start = .;

        KEEP(*(.host_ocram))
        . = ALIGN(8);
        KEEP(*(.fault))

        . = ALIGN(0x1000);
        _host_shared_ocram_end = .;
    } > OCRAM




    /* ---------------------------------------------------------------------
     * Initialized data
     * --------------------------------------------------------------------- */

    .data ALIGN(0x1000) :
    {
        _data_start = .;

        *(.data*)

        . = ALIGN(4);

        _data_end = .;
    } > OCRAM

    _data_size = _data_end - _data_start;



    /* ---------------------------------------------------------------------
     * Zero-initialized data
     * --------------------------------------------------------------------- */

    .bss :
    {
        _bss_start = .;

        *(.bss*)
        *(COMMON)

        . = ALIGN(4);

        _bss_end = .;
    } > OCRAM

    _bss_size = _bss_end - _bss_start;



    /* ---------------------------------------------------------------------
     * OCRAM stacks
     *
     * top-down:
     *
     *     UND
     *     ABT
     *     FIQ
     *     IRQ
     *     SVC
     *     SYS
     * --------------------------------------------------------------------- */

    /* CPU0 bank: top of OCRAM, growing down. */
    _cpu0_und_stack_top = ORIGIN(OCRAM) + LENGTH(OCRAM) - 4;
    _cpu0_abt_stack_top = _cpu0_und_stack_top - _und_stack_size;
    _cpu0_fiq_stack_top = _cpu0_abt_stack_top - _abt_stack_size;
    _cpu0_irq_stack_top = _cpu0_fiq_stack_top - _stack_size;
    _cpu0_svc_stack_top = _cpu0_irq_stack_top - _stack_size;
    _cpu0_sys_stack_top = _cpu0_svc_stack_top - _stack_size;

    /* CPU1 bank: directly below CPU0's, identical sizes. */
    _cpu1_und_stack_top = _cpu0_sys_stack_top - _stack_size;
    _cpu1_abt_stack_top = _cpu1_und_stack_top - _und_stack_size;
    _cpu1_fiq_stack_top = _cpu1_abt_stack_top - _abt_stack_size;
    _cpu1_irq_stack_top = _cpu1_fiq_stack_top - _stack_size;
    _cpu1_svc_stack_top = _cpu1_irq_stack_top - _stack_size;
    _cpu1_sys_stack_top = _cpu1_svc_stack_top - _stack_size;



    /* =====================================================================
     * Link-time static assertions -- every invariant the memory map must hold,
     * grouped here at the bottom. A violation fails the link with the message.
     * ===================================================================== */

    /* Table alignments the hardware demands: TTBR0 (TTBCR.N=0) needs the L1 table
       16384-byte aligned; an L1 pointer descriptor holds the L2 base in bits [31:10],
       so the L2 table must be 1024-byte aligned. */
    ASSERT((_l1_table_base & (0x4000 - 1)) == 0,
        "L1 table is not 16384-byte aligned (TTBR0 requirement)")
    ASSERT((_l2_table_base & (0x400 - 1)) == 0,
        "L2 table is not 1024-byte aligned (L1 pointer descriptor requirement)")

    /* MMU tables must not run into the heap (kMalloc would hand out table memory), and
       the heap must fit inside the DDR. */
    ASSERT(_mmu_tables_end <= _heap_start,
        "MMU tables overlap the heap")
    ASSERT(_heap_end <= ORIGIN(SDRAM) + _sdram_length,
        "heap extends past the end of DDR")

    /* The MMU L2 split maps [_itext_start, _itext_end) cacheable and every other OCRAM
       sub-block device, at 4096-byte granularity. Each boundary must be 4096-byte aligned
       or one page straddles cacheable code and device data -> uncached code, or cached
       (JTAG-incoherent) data. */
    /* Self-boot: the reset vector MUST be at the boot-ROM landing (ORIGIN(OCRAM)),
       and the mkpimage header hole MUST be at image offset 0x40. */
    ASSERT(_itext_start == ORIGIN(OCRAM),
        "reset vector table is not at the OCRAM base (boot ROM branches to ORIGIN(OCRAM))")
    ASSERT(_preloader_header == ORIGIN(OCRAM) + 0x40,
        "mkpimage header hole is not at image offset 0x40")

    ASSERT((_host_shared_ocram_start & (0x1000 - 1)) == 0,
        "host_ocram island does not start on a 4096-byte page boundary")
    ASSERT((_host_shared_ocram_end & (0x1000 - 1)) == 0,
        "host_ocram island does not end on a 4096-byte page boundary")

    /* SDRAM host_shared occupies one 1MB block that build_tables() maps Device;
       it must be 1MB-aligned, clear of the heap, one block wide, and inside DDR. */
    ASSERT((_host_shared_sdram_start & (0x100000 - 1)) == 0,
        "SDRAM host_shared does not start on a 1MB block boundary")
    ASSERT(_host_shared_sdram_start >= _heap_end,
        "SDRAM host_shared overlaps the heap")
    ASSERT((_host_shared_sdram_end - _host_shared_sdram_start) <= 0x100000,
        "SDRAM host_shared exceeds its 1MB Device block")
    ASSERT(_host_shared_sdram_end <= ORIGIN(SDRAM) + _sdram_length,
        "SDRAM host_shared extends past the end of DDR")
    ASSERT((_itext_start & (0x1000 - 1)) == 0,
        "itext (code) does not start on a 4096-byte page boundary")
    ASSERT((_itext_end & (0x1000 - 1)) == 0,
        "itext (code) does not end on a 4096-byte page boundary")
    ASSERT((_data_start & (0x1000 - 1)) == 0,
        "data does not start on a 4096-byte page boundary")

    /* The runtime OCRAM footprint (image through .bss) must not collide with the
       stacks, which grow down from the top. Lowest stack is now CPU1's SYS bank. */
    ASSERT(_bss_end <= _cpu1_sys_stack_top - _stack_size,
        "OCRAM image + CPU0/CPU1 stacks exceed the 64K OCRAM")

    /* Boot ROM copies the loaded image (vectors + code + rodata + data) into the
       64 KB OCRAM window; assert it fits that budget. */
    ASSERT(_data_end - ORIGIN(OCRAM) <= 64K,
        "OCRAM loaded image exceeds the 64K boot-ROM budget")

}
