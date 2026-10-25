OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_reset_handler)

/* -------------------------------------------------------------------------
 * QEMU vexpress-a9 map. One flat DRAM region at 0x60000000 (QEMU's A9 DRAM
 * base); pass -m 256M to size it. Unlike the DE1 (split OCRAM + calibrated
 * SDRAM), here everything -- code/data/bss/stacks/heap/host_shared -- lives in
 * that one RAM, and QEMU's -kernel loader drops the ELF straight in.
 *
 * Intended first bring-up config: single core, MMU/caches off
 *   (ENABLE_MMU=0 ENABLE_DCACHE=0 ENABLE_ICACHE=0 ENABLE_L2=0 ENABLE_SMP=0),
 * so none of the Cyclone-V map-specific paths (SCU, PL310, build_tables,
 * reset-manager secondary release) are exercised. host_shared is plain RAM
 * here -- gdb reads it directly, so no device/uncached mapping is needed; the
 * symbols exist only so the shared firmware links + zeroes them at boot.
 * ------------------------------------------------------------------------- */

_ram_origin = 0x60000000;
_ram_length = 0x10000000;   /* 268435456 bytes (256 MB); match qemu -m 256M */

MEMORY
{
    RAM (rwx) : ORIGIN = 0x60000000, LENGTH = _ram_length
}

/* Compat symbols the shared BSP declares (unused when ENABLE_MMU=0). */
_ocram_origin = _ram_origin;
_sdram_length = _ram_length;

_stack_size     = 0x400;
_abt_stack_size = 0x200;
_und_stack_size = 0x200;

_heap_size = 0x01000000;    /* 16777216 bytes, same as DE1 */


SECTIONS
{
    . = _ram_origin;

    .itext : ALIGN(8)
    {
        _itext_start = .;
        KEEP(*(._vectors))        /* reset vector table must lead (QEMU enters at ELF entry) */
        KEEP(*(.boot_entry))
        *(.text*)
        *(.rodata*)
        . = ALIGN(8);
        _itext_end = .;
    } > RAM

    .data : ALIGN(8)
    {
        _data_start = .;
        *(.data*)
        . = ALIGN(8);
        _data_end = .;
    } > RAM
    _data_size = _data_end - _data_start;

    .bss (NOLOAD) : ALIGN(8)
    {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM
    _bss_size = _bss_end - _bss_start;

    /* OCRAM host island on DE1 -> just RAM here. Holds crash records + boot/SMP gates. */
    .host_ocram (NOLOAD) : ALIGN(8)
    {
        _host_shared_ocram_start = .;
        KEEP(*(.host_ocram))
        . = ALIGN(8);
        KEEP(*(.fault))
        . = ALIGN(8);
        _host_shared_ocram_end = .;
    } > RAM

    /* Bulk telemetry (DE1: 1MB Device SDRAM block) -> just RAM here. */
    .telemetry (NOLOAD) : ALIGN(8)
    {
        _host_shared_sdram_start = .;
        KEEP(*(.telemetry))
        . = ALIGN(8);
        _host_shared_sdram_end = .;
    } > RAM

    /* MMU tables: unused with ENABLE_MMU=0, but keep the symbols + reservation. */
    .mmu_tables (NOLOAD) : ALIGN(16K)
    {
        _l1_table_base = .;
        . = . + 4096 * 4;
        . = ALIGN(1K);
        _l2_table_base = .;
        . = . + 256 * 4;
        _mmu_tables_end = .;
    } > RAM

    .heap (NOLOAD) : ALIGN(8)
    {
        _heap_start = .;
        . = . + _heap_size;
        _heap_end = .;
    } > RAM


    . = ALIGN(8);
    _bank_size = _und_stack_size + _abt_stack_size + _stack_size + _stack_size + _stack_size + _stack_size;
    . = . + 2 * _bank_size;
    _stacks_top = .;

    _cpu0_und_stack_top = _stacks_top - 4;
    _cpu0_abt_stack_top = _cpu0_und_stack_top - _und_stack_size;
    _cpu0_fiq_stack_top = _cpu0_abt_stack_top - _abt_stack_size;
    _cpu0_irq_stack_top = _cpu0_fiq_stack_top - _stack_size;
    _cpu0_svc_stack_top = _cpu0_irq_stack_top - _stack_size;
    _cpu0_sys_stack_top = _cpu0_svc_stack_top - _stack_size;

    _cpu1_und_stack_top = _cpu0_sys_stack_top - _stack_size;
    _cpu1_abt_stack_top = _cpu1_und_stack_top - _und_stack_size;
    _cpu1_fiq_stack_top = _cpu1_abt_stack_top - _abt_stack_size;
    _cpu1_irq_stack_top = _cpu1_fiq_stack_top - _stack_size;
    _cpu1_svc_stack_top = _cpu1_irq_stack_top - _stack_size;
    _cpu1_sys_stack_top = _cpu1_svc_stack_top - _stack_size;


    ASSERT(_heap_end <= _ram_origin + _ram_length, "image + heap exceed QEMU RAM")
    ASSERT(_stacks_top <= _ram_origin + _ram_length, "stacks exceed QEMU RAM")
}
