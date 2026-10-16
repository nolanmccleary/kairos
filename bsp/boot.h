#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>
#include "board.h"

/* Clock Manager: 0xFFD04000 */
#define CM_BASE                 0xFFD04000U
#define CM_R(off)               (*(volatile uint32_t*)(CM_BASE + (off)))

#define CM_CTRL                 CM_R(0x00)
#define CM_BYPASS               CM_R(0x04)
#define CM_INTER                CM_R(0x08)
#define CM_STAT                 CM_R(0x14)

#define CM_MAINPLL_VCO          CM_R(0x40)
#define CM_MAINPLL_MPUCLK       CM_R(0x48)
#define CM_MAINPLL_MAINCLK      CM_R(0x4C)
#define CM_MAINPLL_DBGATCLK     CM_R(0x50)
#define CM_MAINPLL_MAINQSPICLK  CM_R(0x54)
#define CM_MAINPLL_MAINNANDSDMMCCLK CM_R(0x58)
#define CM_MAINPLL_CFGS2FUSER0CLK  CM_R(0x5C)
#define CM_MAINPLL_EN           CM_R(0x60)
#define CM_MAINPLL_MAINDIV      CM_R(0x64)
#define CM_MAINPLL_DBGDIV       CM_R(0x68)
#define CM_MAINPLL_TRACEDIV     CM_R(0x6C)
#define CM_MAINPLL_L4SRC        CM_R(0x70)

#define CM_PERPLL_VCO           CM_R(0x80)
#define CM_PERPLL_EMAC0CLK      CM_R(0x88)
#define CM_PERPLL_EMAC1CLK      CM_R(0x8C)
#define CM_PERPLL_PERQSPICLK    CM_R(0x90)
#define CM_PERPLL_PERNANDSDMMCCLK CM_R(0x94)
#define CM_PERPLL_PERBASECLK    CM_R(0x98)
#define CM_PERPLL_S2FUSER1CLK   CM_R(0x9C)
#define CM_PERPLL_EN            CM_R(0xA0)
#define CM_PERPLL_DIV           CM_R(0xA4)
#define CM_PERPLL_GPIODIV       CM_R(0xA8)
#define CM_PERPLL_SRC           CM_R(0xAC)

#define CM_SDRPLL_VCO           CM_R(0xC0)
#define CM_SDRPLL_DDRDQSCLK     CM_R(0xC8)
#define CM_SDRPLL_DDR2XDQSCLK  CM_R(0xCC)
#define CM_SDRPLL_DDRDQCLK      CM_R(0xD0)
#define CM_SDRPLL_S2FUSER2CLK   CM_R(0xD4)
#define CM_SDRPLL_EN            CM_R(0xD8)

#define CM_ALTR_MPUCLK          CM_R(0xE0)

#define CM_BYPASS_PERPLL        (1U << 3)
#define CM_BYPASS_SDRPLL        (1U << 1)
#define CM_BYPASS_MAINPLL       (1U << 0)

#define CM_INTER_MAINPLLLOST    (1U << 3)
#define CM_INTER_PERPLLLOST     (1U << 4)
#define CM_INTER_SDRPLLLOST     (1U << 5)
#define CM_INTER_MAINPLLLOCKED  (1U << 6)
#define CM_INTER_PERPLLLOCKED   (1U << 7)
#define CM_INTER_SDRPLLLOCKED   (1U << 8)
#define CM_LOCKED_MASK          (CM_INTER_MAINPLLLOCKED | CM_INTER_PERPLLLOCKED | CM_INTER_SDRPLLLOCKED)

#define CM_STAT_BUSY            (1U << 0)
#define CM_CTRL_SAFEMODE        (1U << 0)
#define CM_VCO_EN               (1U << 1)
#define CM_VCO_OUTRESETALL      (1U << 24)
#define CM_VCO_REGEXTSEL        (1U << 31)
#define CM_VCO_RESET_VALUE      0x8001000DU

#define CM_MAINPLL_EN_L4MPCLK       (1U << 2)
#define CM_MAINPLL_EN_DBGATCLK      (1U << 4)
#define CM_MAINPLL_EN_DBGCLK        (1U << 5)
#define CM_MAINPLL_EN_DBGTRACECLK   (1U << 6)
#define CM_MAINPLL_EN_DBGTIMERCLK   (1U << 7)
#define CM_MAINPLL_EN_S2FUSER0CLK   (1U << 9)
#define CM_PERPLL_EN_NANDCLK        (1U << 10)

#define CM_SDRPLL_PHASE_MASK        0x00000E00U

/* SDR Controller: 0xFFC25000 (SOCFPGA_SDR_ADDRESS=0xFFC20000 | 0x5000) */
#define SDR_BASE                0xFFC25000U
#define SDR_R(off)              (*(volatile uint32_t*)(SDR_BASE + (off)))

#define SDR_CTRLCFG             SDR_R(0x000)
#define SDR_DRAMTIMING1         SDR_R(0x004)
#define SDR_DRAMTIMING2         SDR_R(0x008)
#define SDR_DRAMTIMING3         SDR_R(0x00C)
#define SDR_DRAMTIMING4         SDR_R(0x010)
#define SDR_LOWPWRTIMING        SDR_R(0x014)
#define SDR_DRAMODT             SDR_R(0x018)
#define SDR_DRAMADDRW           SDR_R(0x02C)
#define SDR_DRAMIFWIDTH         SDR_R(0x030)
#define SDR_DRAMDEVWIDTH        SDR_R(0x034)
#define SDR_STATICCFG           SDR_R(0x05C)
#define SDR_CTRLWIDTH           SDR_R(0x060)
#define SDR_CPORTWIDTH          SDR_R(0x064)
#define SDR_CPORTWMAP           SDR_R(0x068)
#define SDR_CPORTRMAP           SDR_R(0x06C)
#define SDR_RFIFOCMAP           SDR_R(0x070)
#define SDR_WFIFOCMAP           SDR_R(0x074)
#define SDR_CPORTRDWR           SDR_R(0x078)
#define SDR_LOWPWR_EQ           SDR_R(0x054)
#define SDR_PROTPORT_DEFAULT    SDR_R(0x08C)
#define SDR_PROT_RULE_ADDR      SDR_R(0x090)
#define SDR_PROT_RULE_ID        SDR_R(0x094)
#define SDR_PROT_RULE_DATA      SDR_R(0x098)
#define SDR_PROT_RULE_RDWR      SDR_R(0x09C)
#define SDR_MP_PRIORITY         SDR_R(0x0AC)
#define SDR_MP_WEIGHT0          SDR_R(0x0B0)
#define SDR_MP_WEIGHT1          SDR_R(0x0B4)
#define SDR_MP_WEIGHT2          SDR_R(0x0B8)
#define SDR_MP_WEIGHT3          SDR_R(0x0BC)
#define SDR_MP_PACING0          SDR_R(0x0C0)
#define SDR_MP_PACING1          SDR_R(0x0C4)
#define SDR_MP_PACING2          SDR_R(0x0C8)
#define SDR_MP_PACING3          SDR_R(0x0CC)
#define SDR_MP_THRESHOLD0       SDR_R(0x0D0)
#define SDR_MP_THRESHOLD1       SDR_R(0x0D4)
#define SDR_MP_THRESHOLD2       SDR_R(0x0D8)
#define SDR_PHYCTRL0            SDR_R(0x150)

#define SDR_STATICCFG_APPLYCFG  (1U << 3)

/* Scan Manager: 0xFFF02000 */
#define SCANMGR_BASE            0xFFF02000U
#define SCANMGR_STAT            (*(volatile uint32_t*)(SCANMGR_BASE + 0x00))
#define SCANMGR_EN              (*(volatile uint32_t*)(SCANMGR_BASE + 0x04))
/* padding at 0x08, 0x0C */
#define SCANMGR_FIFO_SINGLE     (*(volatile uint32_t*)(SCANMGR_BASE + 0x10))
#define SCANMGR_FIFO_DOUBLE     (*(volatile uint32_t*)(SCANMGR_BASE + 0x14))
/* triple byte at 0x18 */
#define SCANMGR_FIFO_QUAD       (*(volatile uint32_t*)(SCANMGR_BASE + 0x1C))

#define SCANMGR_STAT_ACTIVE     (1U << 31)
#define SCANMGR_STAT_WFIFOCNT   0x70000000U

/* Reset Manager: 0xFFD05000 */
#define RSTMGR_BASE             0xFFD05000U
#define RSTMGR_MPUMODRST        (*(volatile uint32_t*)(RSTMGR_BASE + 0x10U))
#define RSTMGR_MPUMODRST_CPU1   (1U << 1)   /* MPU core 1 reset. NOTE: bit 0 is core 0 (this core) --
                                               only ever RMW bit 1, never blanket-write this register */
#define RSTMGR_PERMODRST        (*(volatile uint32_t*)(RSTMGR_BASE + 0x14U))
#define RSTMGR_PERMODRST_SDR    (1U << 29)
#define RSTMGR_PERMODRST_L4WD0  (1U << 6)   /* L4 watchdog 0 module reset */
#define RSTMGR_PERMODRST_L4WD1  (1U << 7)   /* L4 watchdog 1 module reset */

/* System Manager: 0xFFD08000 */
#define SYSMGR_BASE             0xFFD08000U

/* Freeze controller (SYSMGR+0x40):
 *   vioctrl(+0x00), padding(+0x04..0x0C), hioctrl(+0x10), src(+0x14)
 */
#define FRZCTRL_HIOCTRL             (*(volatile uint32_t*)(SYSMGR_BASE + 0x50U))
#define FRZCTRL_SRC                 (*(volatile uint32_t*)(SYSMGR_BASE + 0x54U))
#define FRZCTRL_HIOCTRL_OCT_CALSTART    (1U << 8)
#define FRZCTRL_HIOCTRL_REGRST          (1U << 7)
#define FRZCTRL_HIOCTRL_OCTRST          (1U << 6)
#define FRZCTRL_HIOCTRL_DLLRST          (1U << 5)
#define FRZCTRL_HIOCTRL_SLEW            (1U << 4)
#define FRZCTRL_HIOCTRL_WKPULLUP        (1U << 3)
#define FRZCTRL_HIOCTRL_TRISTATE        (1U << 2)
#define FRZCTRL_HIOCTRL_BUSHOLD         (1U << 1)
#define FRZCTRL_HIOCTRL_CFG             (1U << 0)

/* ROM code group control: warmrstcfgio lets IOCSR writes take effect */
#define SYSMGR_ROMCODE_CTRL         (*(volatile uint32_t*)(SYSMGR_BASE + 0xC0U))
#define SYSMGR_ROMCODE_WARMRSTCFGIO (1U << 1)

/* romcodegrp.cpu1startaddr (+0xC4): the boot ROM's CPU1 start-address comparator.
 * On release from reset, CPU1 runs the ROM, which reads this word and branches to
 * it. Gen5 offset 0xC4 (U-Boot romcodegrp.ctrl=0xC0; Linux DT cpu1-start-addr &
 * 0xFF = 0xC4). Write the full entry address, no value mask. Must match the .equ
 * SYSMGR_ROMCODE_CPU1STARTADDR in kernel/kernel.s. */
#define SYSMGR_ROMCODE_CPU1STARTADDR (*(volatile uint32_t*)(SYSMGR_BASE + 0xC4U))

/* NIC-301 L3 interconnect remap: bit 0 = mpuzero (route 0x0..SDRAM_SIZE to SDRAM) */
#define NIC301_REMAP            (*(volatile uint32_t*)0xFF800000U)

/* Cortex-A9 MPCore SCU (Snoop Control Unit) at PERIPHBASE. Keeps the two cores' L1
 * D-caches coherent; invalidate its tag RAM then enable it before caches/coherency come
 * on. Bit 0 = 1 ENABLES (confirmed against Linux smp_scu.c; the ARM TRM had a doc-
 * revision ambiguity about this bit). PERIPHBASE checks out: SCU+0x1000 = GICD (0xFFFED000). */
#define SCU_BASE                BOARD_PERIPHBASE   /* SCU sits at PERIPHBASE+0 (DE1 0xFFFEC000, QEMU 0x1E000000) */
#define SCU_CTRL                (*(volatile uint32_t *)(SCU_BASE + 0x00U))
#define SCU_INVALIDATE_ALL      (*(volatile uint32_t *)(SCU_BASE + 0x0CU))
#define SCU_CTRL_ENABLE         (1U << 0)
#define SCU_INVALIDATE_ALL_WAYS 0xFFFFU   /* 4 ways x up to 4 CPUs */

/* PL310 (L2C-310) L2 cache controller. Register offsets and the RAM-latency field
 * encoding are from the ARM L2C-310 TRM; the Cyclone V RAM latencies match the Linux
 * socfpga device tree (tag 1-1-1, data 2-1-1 cycles). aux/latency are writable only
 * while the cache is disabled. */
#define PL310_BASE              0xFFFEF000U
#define PL310_CTRL              (*(volatile uint32_t *)(PL310_BASE + 0x100U))
#define PL310_AUX_CTRL          (*(volatile uint32_t *)(PL310_BASE + 0x104U))
#define PL310_TAG_LATENCY       (*(volatile uint32_t *)(PL310_BASE + 0x108U))
#define PL310_DATA_LATENCY      (*(volatile uint32_t *)(PL310_BASE + 0x10CU))
#define PL310_INV_WAY           (*(volatile uint32_t *)(PL310_BASE + 0x77CU))
#define PL310_CLEAN_INV_WAY     (*(volatile uint32_t *)(PL310_BASE + 0x7FCU))
#define PL310_FILTER_START      (*(volatile uint32_t *)(PL310_BASE + 0xC00U))
#define PL310_FILTER_END        (*(volatile uint32_t *)(PL310_BASE + 0xC04U))

#define PL310_CTRL_ENABLE           (1U << 0)
#define PL310_AUX_SHARED_OVERRIDE   (1U << 22)  /* cache SHAREABLE memory in L2 (else it isn't) */
#define PL310_AUX_REPLACE_ROUNDROBIN (1U << 25) /* 0 = pseudo-random, 1 = round-robin */
#define PL310_AUX_DATA_PREFETCH     (1U << 28)
#define PL310_AUX_INSTR_PREFETCH    (1U << 29)

/* Cyclone V L2 is 8-way, so 8 way-mask bits for the by-way maintenance ops. */
#define PL310_ALL_WAYS          0xFFU

/* Tag RAM 1/1/1 and Data RAM read=2,write=1,setup=1 cycles. Field = (cycles - 1),
 * setup at [2:0], read at [6:4], write at [10:8]. */
#define PL310_TAG_LATENCY_VAL   0x00000000U
#define PL310_DATA_LATENCY_VAL  0x00000010U

void pll_init(void);
void scan_mgr_init(void);
void sdram_ctrl_init(void);

#endif /* BOOT_H */
