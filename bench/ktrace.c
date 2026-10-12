#include "ktrace.h"


#ifdef MODE_TEST


/* Debugger WRITES these, CPU reads them -- a mirror can't serve a host write
 * into a cached read, so they must be plain-uncached (host_shared). Set at
 * runtime before use; the NOLOAD region isn't zeroed at boot. */
HOST_SHARED_OCRAM volatile uint32_t g_test_release;


#define KTRACE_BP_ATTR \
    __attribute__((noinline, noclone, used, aligned(4)))


KTRACE_BP_ATTR
void ktrace_bp_alloc_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_edf_ready(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_edf_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_balance_ready(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_balance_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_micro_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_suspend_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


void ktrace_wait_release(void)
{
    g_test_release = 0u;

    __asm__ __volatile__(
        "dmb sy"
        :
        :
        : "memory"
    );


    while (g_test_release == 0u)
    {
        __asm__ __volatile__(
            ""
            :
            :
            : "memory"
        );
    }


    __asm__ __volatile__(
        "dmb sy"
        :
        :
        : "memory"
    );


    g_test_release = 0u;
}


#if BOOT_TEST

/* Self-boot gate: spin until JTAG writes g_boot_release (test.py --bootable).
 * Zeroed on entry so an uninitialized/garbage value can't skip the gate; runs
 * before c_startup(), i.e. MMU + caches off, so the flag is plain physical
 * memory the debugger reads/writes coherently. */
HOST_SHARED_OCRAM volatile uint32_t g_boot_release;

void ktrace_wait_boot(void)
{
    g_boot_release = 0u;

    __asm__ __volatile__(
        "dmb sy"
        :
        :
        : "memory"
    );


    while (g_boot_release == 0u)
    {
        __asm__ __volatile__(
            ""
            :
            :
            : "memory"
        );
    }


    __asm__ __volatile__(
        "dmb sy"
        :
        :
        : "memory"
    );
}

#endif


#endif
