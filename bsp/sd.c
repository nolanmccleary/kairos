#include <stdint.h>

#include "sd.h"


/* -------------------------------------------------------------------------
 * SD/MMC app-code loader.
 *
 * The Cyclone V boot ROM only ever loads the preloader (<=~60KB) into OCRAM and
 * never touches SDRAM, so any code that lives in SDRAM has to be pulled there by
 * the running preloader. After cpu0_startup brings SDRAM up, load_from_sd() reads
 * the application/test text from the card into the SDRAM .appcode region; startup
 * then vectors into main() (which stays in OCRAM) and main() calls into it.
 *
 * The card was already initialized by the boot ROM when it read the preloader, so
 * we reuse that state and issue only CMD18 (READ_MULTIPLE_BLOCK) reads via PIO. A
 * standalone init (CMD0/8/ACMD41/2/3/7/16) would go here if that assumption ever
 * breaks.
 * ------------------------------------------------------------------------- */

/* DesignWare DWC_mshc on the Cyclone V HPS. */
#define SDMMC_BASE      0xFF704000u

#define SDMMC_CTRL      0x00u
#define SDMMC_BLKSIZ    0x1Cu
#define SDMMC_BYTCNT    0x20u
#define SDMMC_CMDARG    0x28u
#define SDMMC_CMD       0x2Cu
#define SDMMC_RINTSTS   0x44u
#define SDMMC_STATUS    0x48u
#define SDMMC_FIFODATA  0x200u   /* IP-version dependent; 0x200 on this controller */

#define SD_R(off)       (*(volatile uint32_t*)(SDMMC_BASE + (off)))

/* CTRL */
#define CTRL_FIFO_RESET     (1u << 1)

/* CMD */
#define CMD_INDEX(n)        ((n) & 0x3Fu)
#define CMD_RESP_EXPECT     (1u << 6)
#define CMD_CHECK_CRC       (1u << 8)
#define CMD_DATA_EXPECTED   (1u << 9)
#define CMD_SEND_AUTO_STOP  (1u << 12)
#define CMD_WAIT_PRVDATA    (1u << 13)
#define CMD_USE_HOLD_REG    (1u << 29)
#define CMD_START           (1u << 31)

/* RINTSTS */
#define INT_CMD_DONE        (1u << 2)
#define INT_DTO             (1u << 3)   /* data transfer over */
#define INT_RXDR            (1u << 5)
#define INT_ERROR           ((1u << 1) | (1u << 6) | (1u << 7) | (1u << 8) | \
                             (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | \
                             (1u << 13) | (1u << 15))

/* STATUS: FIFO occupancy (word count) is bits [29:17]. */
#define STATUS_FIFO_COUNT(s)  (((s) >> 17) & 0x1FFFu)

#define SD_BLOCK_SIZE   512u

/* Sector the app-code blob is written to on the card. TODO: wire flash_sd.sh to
 * place app.bin here once bench/*.o is relocated into the SDRAM .appcode region. */
#define SD_APPCODE_LBA  0u


/* Linker-defined bounds of the SDRAM application-code region (de1-soc.ld). */
extern char _appcode_start[];
extern char _appcode_end[];


/* Make a region that was written through the data side fetchable as code: clean
 * D-cache to the point of unification over the range, then invalidate I-cache and
 * the branch predictor. A9 cache line = 32 bytes. */
static void sync_icache_region(uintptr_t start, uintptr_t end)
{
    for (uintptr_t a = (start & ~31u); a < end; a += 32u)
    {
        __asm__ volatile ("mcr p15, 0, %0, c7, c11, 1" :: "r"(a) : "memory");  /* DCCMVAU */
    }

    __asm__ volatile (
        "dsb\n\t"
        "mov r0, #0\n\t"
        "mcr p15, 0, r0, c7, c5, 0\n\t"   /* ICIALLU: invalidate entire I-cache */
        "mcr p15, 0, r0, c7, c5, 6\n\t"   /* BPIALL:  invalidate branch predictor */
        "dsb\n\t"
        "isb\n\t"
        ::: "r0", "memory"
    );
}


void load_from_sd(void)
{
    uint32_t size = (uint32_t)(_appcode_end - _appcode_start);
    if (size == 0u)
    {
        return;   /* no app-code relocated to SDRAM yet */
    }

    uint32_t blocks = (size + SD_BLOCK_SIZE - 1u) / SD_BLOCK_SIZE;
    uint32_t words  = (blocks * SD_BLOCK_SIZE) / 4u;

    /* Flush the FIFO (card state carried over from the boot ROM). */
    SD_R(SDMMC_CTRL) |= CTRL_FIFO_RESET;
    while (SD_R(SDMMC_CTRL) & CTRL_FIFO_RESET) { }

    SD_R(SDMMC_RINTSTS) = 0xFFFFFFFFu;            /* clear stale status */
    SD_R(SDMMC_BLKSIZ)  = SD_BLOCK_SIZE;
    SD_R(SDMMC_BYTCNT)  = blocks * SD_BLOCK_SIZE;
    SD_R(SDMMC_CMDARG)  = SD_APPCODE_LBA;         /* SDHC: block (sector) address */

    SD_R(SDMMC_CMD) = CMD_START | CMD_USE_HOLD_REG | CMD_WAIT_PRVDATA
                    | CMD_SEND_AUTO_STOP | CMD_DATA_EXPECTED
                    | CMD_CHECK_CRC | CMD_RESP_EXPECT | CMD_INDEX(18u);

    while (SD_R(SDMMC_CMD) & CMD_START) { }        /* wait for the controller to accept it */

    volatile uint32_t* dst = (volatile uint32_t*)_appcode_start;
    uint32_t got = 0u;
    while (got < words)
    {
        uint32_t st = SD_R(SDMMC_RINTSTS);
        if (st & INT_ERROR)
        {
            break;   /* TODO: surface the failing RINTSTS bits to a fault record */
        }

        uint32_t avail = STATUS_FIFO_COUNT(SD_R(SDMMC_STATUS));
        while (avail-- > 0u && got < words)
        {
            dst[got++] = SD_R(SDMMC_FIFODATA);
        }
    }

    while (!(SD_R(SDMMC_RINTSTS) & INT_DTO)) { }   /* wait for data-transfer-over */
    SD_R(SDMMC_RINTSTS) = 0xFFFFFFFFu;

    sync_icache_region((uintptr_t)_appcode_start, (uintptr_t)_appcode_end);
}
