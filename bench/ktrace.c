#include "ktrace.h"
#include "cpu.h"
#include "telemetry.h"


#ifdef MODE_TEST


/* Debugger WRITES these, CPU reads them -- a mirror can't serve a host write
 * into a cached read, so they must be plain-uncached (host_shared). Set at
 * runtime before use; the NOLOAD region isn't zeroed at boot. */
OCRAM_UNCACHED_TELEMETRY volatile uint32_t g_test_release;


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


KTRACE_BP_ATTR
void ktrace_bp_persist_ready(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_persist_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


KTRACE_BP_ATTR
void ktrace_bp_sched_sweep_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


void ktrace_bp_tickirq_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


void ktrace_bp_gantt_done(void)
{
    __asm__ __volatile__(
        "nop"
        :
        :
        : "memory"
    );
}


/* Trace infra lives in workload_edf.c and is reused by the gantt run. */
extern uint32_t trace_active;
extern volatile uint32_t g_trace_len[NUM_CPUS];

#define KTRACE_IRQ_EVENTS_MAX 512u

/* One row per tick-IRQ fire. Parallel arrays (no new struct): x-index aligns with the
 * thread trace (g_trace_len at fire time), plus core and IRQ id. */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_irq_ev_x[KTRACE_IRQ_EVENTS_MAX];
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_irq_ev_core[KTRACE_IRQ_EVENTS_MAX];
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_irq_ev_id[KTRACE_IRQ_EVENTS_MAX];
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_irq_ev_cycles[KTRACE_IRQ_EVENTS_MAX];
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_irq_ev_count;

void ktrace_irq_fire(uint32_t core, uint32_t id, uint32_t cycles)
{
    if (!trace_active) return;

    uint32_t n = g_irq_ev_count;
    if (n >= KTRACE_IRQ_EVENTS_MAX) return;

    g_irq_ev_x[n]      = g_trace_len[core];   /* same index the thread hook uses this tick */
    g_irq_ev_core[n]   = core;
    g_irq_ev_id[n]     = id;
    g_irq_ev_cycles[n] = cycles;
    g_irq_ev_count     = n + 1u;
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
OCRAM_UNCACHED_TELEMETRY volatile uint32_t g_boot_release;

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
