#include <stdbool.h>
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

/* Register offsets -- verified against DesignWare dw_mmc (U-Boot include/dwmmc.h). */
#define SDMMC_CTRL      0x00u
#define SDMMC_BLKSIZ    0x1Cu
#define SDMMC_BYTCNT    0x20u
#define SDMMC_CMDARG    0x28u
#define SDMMC_CMD       0x2Cu
#define SDMMC_RINTSTS   0x44u
#define SDMMC_STATUS    0x48u
#define SDMMC_BMOD      0x80u
#define SDMMC_FIFODATA  0x200u   /* dw_mmc DATA register is fixed at 0x200 on this IP */

#define SD_R(off)       (*(volatile uint32_t*)(SDMMC_BASE + (off)))

/* CTRL bits (DWMCI_CTRL_*). */
#define CTRL_FIFO_RESET     (1u << 1)
#define CTRL_DMA_EN         (1u << 5)    /* external DMA enable  */
#define CTRL_IDMAC_EN       (1u << 25)   /* internal DMAC enable */

/* BMOD (internal DMAC bus mode). */
#define BMOD_SWR            (1u << 0)    /* software reset of the IDMAC */

/* CMD bits (DWMCI_CMD_*). */
#define CMD_INDEX(n)        ((n) & 0x3Fu)
#define CMD_RESP_EXPECT     (1u << 6)
#define CMD_CHECK_CRC       (1u << 8)
#define CMD_DATA_EXPECTED   (1u << 9)
#define CMD_WAIT_PRVDATA    (1u << 13)
#define CMD_USE_HOLD_REG    (1u << 29)
#define CMD_START           (1u << 31)

#define CMD_READ_SINGLE     17u          /* CMD17 READ_SINGLE_BLOCK */

/* RINTSTS bits (DWMCI_INTMSK_*). ERROR = RE|RCRC|DCRC|RTO|DRTO|HTO|FRUN|HLE|SBE|EBE
 * (bits 1,6-13,15); bit 14 = ACD auto-command-done is NOT an error. */
#define INT_CMD_DONE        (1u << 2)
#define INT_DTO             (1u << 3)   /* data transfer over */
#define INT_ERROR           ((1u << 1) | (1u << 6) | (1u << 7) | (1u << 8) | \
                             (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | \
                             (1u << 13) | (1u << 15))

/* STATUS: FIFO occupancy (word count) is bits [29:17] (DWMCI_FIFO_SHIFT/MASK). */
#define STATUS_FIFO_COUNT(s)  (((s) >> 17) & 0x1FFFu)

#define SD_BLOCK_SIZE   512u
#define SD_BLOCK_WORDS  (SD_BLOCK_SIZE / 4u)

/* app.bin location: not a hardcoded absolute sector (card-layout-dependent).
 * Instead we read the MBR, find the Cyclone V preloader partition (MBR type 0xA2,
 * the one the boot ROM loads us from), and read app.bin from a fixed offset inside
 * it. flash_sd.sh writes app.bin at that same offset. The offset clears the boot
 * ROM's preloader-copy slots (0/64K/128K/192K) so it is never mistaken for one. */
#define MBR_PART_TYPE_A2    0xA2u
#define APP_OFFSET_SECTORS  512u          /* 256 KiB into the 0xA2 partition */


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


/* Read one 512-byte block (CMD17) at `lba` into dst[0..127]. Returns 0 on success,
 * -1 on a command/data error. CMD17 is self-terminating -- no CMD12/auto-stop. */
static int read_block(uint32_t lba, uint32_t* dst)
{
    SD_R(SDMMC_RINTSTS) = 0xFFFFFFFFu;            /* clear stale status */
    SD_R(SDMMC_BLKSIZ)  = SD_BLOCK_SIZE;
    SD_R(SDMMC_BYTCNT)  = SD_BLOCK_SIZE;
    SD_R(SDMMC_CMDARG)  = lba;                    /* SDHC: block (sector) address */

    SD_R(SDMMC_CMD) = CMD_START | CMD_USE_HOLD_REG | CMD_WAIT_PRVDATA
                    | CMD_DATA_EXPECTED | CMD_CHECK_CRC | CMD_RESP_EXPECT
                    | CMD_INDEX(CMD_READ_SINGLE);

    while (SD_R(SDMMC_CMD) & CMD_START) { }        /* START self-clears once the CIU accepts it */

    /* Wait for the command response, then check command errors (RTO/RE/RCRC...). */
    uint32_t st;
    do
    {
        st = SD_R(SDMMC_RINTSTS);
        if (st & INT_ERROR) return -1;
    }   while (!(st & INT_CMD_DONE));

    /* Drain exactly one block of words as the card streams them into the FIFO. */
    uint32_t got = 0u;
    while (got < SD_BLOCK_WORDS)
    {
        st = SD_R(SDMMC_RINTSTS);
        if (st & INT_ERROR) return -1;

        uint32_t avail = STATUS_FIFO_COUNT(SD_R(SDMMC_STATUS));
        while (avail-- > 0u && got < SD_BLOCK_WORDS)
        {
            dst[got++] = SD_R(SDMMC_FIFODATA);
        }
    }

    while (!(SD_R(SDMMC_RINTSTS) & INT_DTO))       /* data-transfer-over */
    {
        if (SD_R(SDMMC_RINTSTS) & INT_ERROR) return -1;
    }
    SD_R(SDMMC_RINTSTS) = 0xFFFFFFFFu;
    return 0;
}


void load_from_sd(void)
{
    uint32_t size = (uint32_t)(_appcode_end - _appcode_start);
    if (size == 0u)
    {
        return;   /* no app-code relocated to SDRAM yet */
    }

    uint32_t blocks = (size + SD_BLOCK_SIZE - 1u) / SD_BLOCK_SIZE;

    /* Force FIFO/PIO mode: the boot ROM read the preloader via the internal DMAC
     * and likely left CTRL.IDMAC_EN/DMA_EN set, which would route card data to a
     * DMA descriptor instead of the FIFO we poll. Clear both, soft-reset the DMAC,
     * then flush the FIFO. Clock/FIFOTH/card selection are reused from the boot ROM. */
    SD_R(SDMMC_CTRL) &= ~(CTRL_IDMAC_EN | CTRL_DMA_EN);
    SD_R(SDMMC_BMOD)  = BMOD_SWR;
    SD_R(SDMMC_CTRL) |= CTRL_FIFO_RESET;
    while (SD_R(SDMMC_CTRL) & CTRL_FIFO_RESET) { }

    /* Find the 0xA2 (preloader) partition's start LBA from the MBR, so app.bin's
     * location is independent of the card's partition layout. */
    static uint32_t mbr[SD_BLOCK_WORDS];
    if (read_block(0u, mbr) != 0) return;

    const uint8_t* m = (const uint8_t*)mbr;
    uint32_t a2_lba = 0u;
    bool found = false;
    for (uint32_t i = 0u; i < 4u; i++)
    {
        const uint8_t* e = m + 0x1BEu + 16u * i;     /* MBR partition entry i */
        if (e[4] == MBR_PART_TYPE_A2)                /* byte 4 = partition type */
        {
            a2_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8)
                   | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);  /* bytes 8-11 = start LBA */
            found = true;
            break;
        }
    }
    if (!found) return;

    uint32_t app_lba = a2_lba + APP_OFFSET_SECTORS;
    uint32_t* dst = (uint32_t*)_appcode_start;
    for (uint32_t b = 0u; b < blocks; b++)
    {
        if (read_block(app_lba + b, dst + b * SD_BLOCK_WORDS) != 0)
        {
            break;   /* TODO: surface the failing block + RINTSTS to a fault record */
        }
    }

    sync_icache_region((uintptr_t)_appcode_start, (uintptr_t)_appcode_end);
}
