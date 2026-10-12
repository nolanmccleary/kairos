#ifndef __SD_H__
#define __SD_H__

/* Copy the SDRAM application-code region from the SD card.
 *
 * Runs from OCRAM (kernel) after SDRAM is up (post cpu0_startup) and before the
 * startup code vectors into main(). The destination is the linker-defined region
 * [_appcode_start, _appcode_end); the source is the raw SD sector SD_APPCODE_LBA.
 * If the region is empty (_appcode_end == _appcode_start) it is a no-op. */
void load_from_sd(void);

#endif
