#include <stddef.h>
#include <stdint.h>

#include "telemetry.h"


/* Uncached OCRAM (linker .host_shared); host reads it by symbol via nm. */
metric_t g_metrics[METRIC_SLOTS] SDRAM_UNCACHED_TELEMETRY;
