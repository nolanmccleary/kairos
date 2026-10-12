.syntax unified
.arm

.equ MODE_USR, 0x10
.equ MODE_FIQ, 0x11
.equ MODE_IRQ, 0x12
.equ MODE_SVC, 0x13
.equ MODE_MON, 0x16
.equ MODE_ABT, 0x17
.equ MODE_HYP, 0x1A
.equ MODE_UND, 0x1B
.equ MODE_SYS, 0x1F
.equ I_BIT, 0x80   @ IRQ disable/mask bit, CPSR[7]
.equ F_BIT, 0x40   @ FIQ disable/mask bit, CPSR[6]

@; CYCLONE V MAPPINGS
.equ GICC_BASE,  BOARD_GICC_BASE    @; board-selected (Makefile --defsym): DE1 0xFFFEC100, QEMU 0x1E000100
.equ GICC_IAR,   GICC_BASE + 0x0C
.equ GICC_EOIR,  GICC_BASE + 0x10

@; Reset Manager MPU module reset (must match RSTMGR_MPUMODRST / _CPU1 in bsp/boot.h).
@; Bit 0 = core 0 (THIS core) -- only ever RMW bit 1; never blanket-write this register.
.equ RSTMGR_MPUMODRST,      0xFFD05010
.equ RSTMGR_MPUMODRST_CPU1, 0x2

@; System Manager romcodegrp.cpu1startaddr -- boot ROM's CPU1 start-address comparator.
@; (must match SYSMGR_ROMCODE_CPU1STARTADDR in bsp/boot.h)
.equ SYSMGR_ROMCODE_CPU1STARTADDR, 0xFFD080C4

@; Peripheral module reset: bits 6/7 hold L4 watchdog 0/1 in reset.
.equ RSTMGR_PERMODRST,      0xFFD05014
.equ RSTMGR_PERMODRST_L4WD, 0xC0   @; (1<<6)|(1<<7)




.section ._vectors, "ax"
.global _vectors
.global _reset_handler
_vectors:
    B _reset_handler @; rst interrupt vectors here by abi contract
    B _undef_handler
    B _swi_handler
    B _prefetch_handler
    B _abort_handler
    NOP               @; Reserved vector
    B _irq_handler
    B _fiq_handler





.section .boot_entry, "ax"
.global _boot_entry
_boot_entry:
    B _reset_handler






.text
_reset_handler:

    ; ldr r0, =RSTMGR_PERMODRST @; Disable watchdog interrupts
    ; ldr r1, [r0]
    ; orr r1, r1, #RSTMGR_PERMODRST_L4WD
    ; str r1, [r0]
    ; dsb


    ldr r0, =_vectors
    mcr p15, 0, r0, c12, c0, 0  @; VBAR = _vectors


    @; SET STACK POINTERS AND CPSR FOR EACH MODE

.if ENABLE_SMP != 0
    mrc p15, 0, r0, c0, c0, 5        @; MPIDR
    and r0, r0, #0x3                 @; core id
    cmp r0, #0
    ldrne r0, =_cpu1_und_stack_top   @; CPU1: its own stack bank
    ldreq r0, =_cpu0_und_stack_top   @; CPU0: top-of-OCRAM bank
.else
    ldr r0, =_cpu0_und_stack_top
.endif
    msr CPSR_c, #(MODE_UND | I_BIT | F_BIT)
    mov sp, r0
    ldr r1, =_und_stack_size
    sub r0, r0, r1

    msr CPSR_c, #(MODE_ABT | I_BIT | F_BIT)
    mov sp, r0
    ldr r1, =_abt_stack_size
    sub r0, r0, r1

    msr CPSR_c, #(MODE_FIQ | I_BIT | F_BIT)
    mov sp, r0
    ldr r1, =_stack_size
    sub r0, r0, r1

    msr CPSR_c, #(MODE_IRQ | I_BIT | F_BIT)
    mov sp, r0
    ldr r1, =_stack_size
    sub r0, r0, r1

    msr CPSR_c, #(MODE_SVC | I_BIT | F_BIT)
    mov sp, r0
    ldr r1, =_stack_size
    sub r0, r0, r1

    msr CPSR_c, #(MODE_SYS | I_BIT | F_BIT)
    mov sp, r0


    ; mrc p15, 0, r1, c1, c0, 0
    ; bic r1, r1, #0x1            @; MMU off
    ; bic r1, r1, #(0x1 << 12)   @; I-cache off
    ; bic r1, r1, #(0x1 << 2)    @; D-cache off
    ; dsb
    ; mcr p15, 0, r1, c1, c0, 0
    ; isb


    @; INVALIDATE L1 I-CACHE
    mov r1, #0
    DSB
    mcr p15, 0, r1, c7, c5, 0
    ISB

    @; WALK D-CACHE (no bulk invalidate — must do set/way)
    mrc p15, 1, r0, c0, c0, 0
    mov r3, #0x1FF
    and r0, r3, r0, lsr #13    @ r0 = numsets - 1
    mov r1, #0

way_loop:
    mov r3, #0
set_loop:
    mov r2, r1, lsl #30
    orr r2, r2, r3, lsl #5
    mcr p15, 0, r2, c7, c6, 2
    add r3, r3, #1
    cmp r0, r3
    BGT set_loop

    add r1, r1, #1
    cmp r1, #4
    BNE way_loop

    @; INVALIDATE TLB
    mcr p15, 0, r1, c8, c7, 0

    @; BRANCH PREDICTION ENABLE
    mov r1, #0
    mrc p15, 0, r1, c1, c0, 0
    orr r1, r1, #(0x1<<11)
    mcr p15, 0, r1, c1, c0, 0

    @; ENABLE D-SIDE PREFETCH
    MRC p15, 0, r1, c1, c0, 1
    ORR r1, r1, #(0x1 <<2)
    MCR p15, 0, r1, c1, c0, 1
    DSB
    ISB

    .if ENABLE_SMP != 0
        mrc p15, 0, r0, c0, c0, 5   @; MPIDR
        and r0, r0, #0x3            @; core id: raw MPIDR is 0x8000000N, mask to 0/1
        cmp r0, #0
        bne _cpu1_fork
    .endif
    


_cpu0_fork:
@; Zero BSS
    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
bss_zero:
    cmp r0, r1
    strlt r2, [r0], #4
    blt bss_zero

@; Zero the OCRAM host island (fault records, boot gate, SMP mailbox)
    ldr r0, =_host_shared_ocram_start
    ldr r1, =_host_shared_ocram_end
    mov r2, #0
host_ocram_zero:
    cmp r0, r1
    strlt r2, [r0], #4
    blt host_ocram_zero

@; RELEASE CPU1 -> it re-enters _reset_handler and forks on MPIDR
.if ENABLE_SMP != 0
 .if BOARD_IS_QEMU != 0
    @; QEMU vexpress-a9 starts BOTH cores at the ELF entry (_reset_handler) -- no pen,
    @; no powered-off secondary, no PSCI. CPU1 self-starts and routes to _cpu1_fork on
    @; MPIDR, so nothing to release here. (Verified: thread 2 sits at _reset_handler.)
    nop
 .else
    ldr r0, =SYSMGR_ROMCODE_CPU1STARTADDR
    ldr r1, =_reset_handler
    str r1, [r0]                        @; cpu1startaddr = &_reset_handler (CPU1 re-enters, forks on MPIDR)

    ldr r0, =RSTMGR_MPUMODRST
    ldr r1, [r0]
    bic r1, r1, #RSTMGR_MPUMODRST_CPU1  @; deassert CPU1 reset -> CPU1 boots ROM -> _cpu1_spin
    str r1, [r0]
 .endif
.endif

.if BOOT_TEST != 0
    bl ktrace_wait_boot
.endif
    bl cpu0_startup         @; RUN CP0 STARTUP AND SET MAILBOX

.if MODE_TEST != 0
    ldr r0, =_host_shared_sdram_start
    ldr r1, =_host_shared_sdram_end
    mov r2, #0
host_shared_sdram_zero:
    cmp r0, r1
    strlt r2, [r0], #4
    blt host_shared_sdram_zero
.endif

    @; cpsie i
    @; ldr r0, =g_cpu_mailbox_uncached
    @; mov r1, #1
    @; str r1, [r0]
    @; dsb
    @; sev
    bl main



_cpu1_fork:
.if ENABLE_SMP != 0
    .align 2
    ldr r0, =g_cpu1_ready
    mov r1, #1
    str r1, [r0]                        @; publish: CPU1 is off the 0x0 alias, in OCRAM
    dsb
    ldr r0, =g_cpu_mailbox_uncached

    wfe
    1:  ldr r1, [r0]
        cmp r1, #0
        bne engage
        wfe
        b 1b

    engage:
        bl cpu1_startup
        cpsie i

        ldr r0, =g_cpu_mailbox_uncached
        mov r1, #2 
        str r1, [r0]  
        dsb 
        sev

        bl cpu1_main
.endif
    


_identify_and_clear_source:
    ldr     r0, =GICC_IAR
    ldr     r1, [r0]            @; acknowledge + get ID
    ldr     r0, =GICC_EOIR
    str     r1, [r0]            @; EOI
    mov     r0, r1              @; arm calling convention returns value in r0; we want to return interrupt ID
    BX      lr




_undef_handler:
    sub lr, lr, #4      @; faulting instruction address
    mov r0, lr          @; arg1 = pc (saved before bl clobbers lr)
    mrs r1, spsr        @; arg2 = pre-fault CPSR
    mov r2, #1          @; arg3 = FAULT_UNDEF
    bl fault_capture
    bl fault_halt
    b .

_swi_handler:
    sub lr, lr, #4
    mov r0, lr
    mrs r1, spsr
    mov r2, #2          @; FAULT_SWI
    bl fault_capture
    bl fault_halt
    b .

_prefetch_handler:
    sub lr, lr, #4
    mov r0, lr
    mrs r1, spsr
    mov r2, #3          @; FAULT_PREFETCH
    bl fault_capture
    bl fault_halt
    b .

_abort_handler:
    sub lr, lr, #8      @; data abort: faulting instr = lr - 8
    mov r0, lr
    mrs r1, spsr
    mov r2, #4          @; FAULT_DATA
    bl fault_capture
    bl fault_halt
    b .


@;REMEMBER: We are in SYS mode when IRQ fires. That means:
@;1) lr_irq stores last SYS PC 
@;2) after going back to sys mode, lr_sys stores last sys LR
_irq_handler:
    sub lr, lr, #4 @; get lr_irq i.e. get the last pre-interrupt PC value
    srsfd sp!, #0x1f @; Store Return State Full Descending --- push cpsr then pc (cached spsr then lr_irq in this case) onto sysmode stack (specified by 0x1f), sp autodecs
    
    cps #0x1f @; Change Processor State --- switch to system mode
    cpsid i @; Change Processor State Interrupt Disable --- no nested interrupts for now

    push {r0-r12} @; store the FULL integer register file. A context switch resumes a
                  @; DIFFERENT task, so r4-r11 (callee-saved, live across the interrupted
                  @; task's calls) must be part of the saved context -- saving only the
                  @; AAPCS caller-saved set corrupts a resumed task's r4-r11.

    push {lr} @; push lr_sys onto sysmode stack

    
    cps #0x12

    BL _identify_and_clear_source @; get irq switch vector and ack interrupt (accepted + finished reading switch vector)
    BL c_irq_handler @; r0 injects arg1 of c func as per ARM ABI, set via identify_and_clear_source --- nominal routine switches sysmode sp from scheduler-managed pointer bank

    cps #0x1f @; switch back to system mode

    pop {lr} @; restore lr_sys

    pop {r0-r12} @; restore the FULL integer register file for the resumed task

    @; NOTE: do NOT re-enable IRQs here. RFEFD restores CPSR (with the thread's
    @; I-bit) atomically with the PC. A `cpsie i` before RFEFD opens a re-entrancy
    @; window: a tick firing in these last instructions re-enters _irq_handler on
    @; the already-restored outgoing SP and corrupts the scheduler state.
    RFEFD sp! @; Set PC and CPSR (re-enables IRQs via restored SPSR)






_fiq_handler:
    sub lr, lr, #4 @; get lr_irq
    srsfd sp!, #0x1f @; save lr_irq and spsr_irq onto sysmode stack; decrement sysmode sp after
    
    cps #0x1f @; switch to system mode
    cpsid if @; no nested interrupts for now

    push {r0-r3, r12} @; store AAPCS regset

    and r1, sp, #4 @; 8-byte align sp
    sub sp, sp, r1 
    push {r1, lr} @; store adjustment and lr_sys

    BL _identify_and_clear_source
    BL c_fiq_handler @; r0 injects arg1 of c func as per ARM ABI, set via identify_and_clear_source

    pop {r1, lr} @; restore lr_sys
    add sp, sp, r1 @; unadjust stack
    pop {r0-r12} @; restore AAPCS regset

    @; NOTE: no cpsie here — RFEFD restores CPSR (I/F bits) atomically with the PC.
    RFEFD sp! @; Set PC and CPSR


