.equ locked, 1
.equ unlocked, 0


.global _lock_mutex_persistent
_lock_mutex_persistent:
    ldr r1, =locked

    1:
        ldrex r2, [r0]
        cmp r2, r1
        wfeeq
        beq 1b

        strex r2, r1, [r0]
        cmpne r2, #0
        bne 1b

        dmb
        bx lr


.global _lock_mutex_best_effort
_lock_mutex_best_effort:
    ldr r1, =locked
    ldrex r2, [r0]
    cmp r2, r1

    movne r3, r0
    strexne r0, r1, [r3]

    moveq r0, #1

    cmp r2, r1
    bne 1f
    clrex

    1:
        cmp r0, #0
        bne 2f
        dmb

    2:
        bx lr


.globl _unlock_mutex
_unlock_mutex:
    ldr r1, =unlocked
    dmb
    str r1, [r0]
    sev
    bx lr

