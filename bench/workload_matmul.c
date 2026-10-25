#include <stdint.h>

#include "telemetry.h"
#include "tlsf.h"
#include "workload_matmul.h"


/* Integer matrix multiply mat_out = mat_a * mat_b, MATMUL_DIM square int32.
 * The three matrices are 3 x 4096 bytes = 12288 bytes, staying resident in the
 * 32768-byte L1 in both builds, so the timed loop is instruction-fetch bound --
 * which is what the I-cache on/off comparison probes. */

#define MATMUL_SLOT  6          /* g_metrics -> "matmul_32" (test.py) */
#define MATMUL_DIM   32
#define MATMUL_REPS  8


/* volatile + a full read-back of mat_out defeats DCE of the multiply and lets
 * the host confirm the result is bit-identical across I-cache on/off builds.
 * SDRAM_UNCACHED_TELEMETRY (uncached OCRAM) so JTAG reads it coherently. */
SDRAM_UNCACHED_TELEMETRY volatile uint32_t g_matmul_checksum;


/* Per-rep cycle counts so the host can watch the I-cache warm up (rep 0 cold). */
SDRAM_UNCACHED_TELEMETRY uint32_t g_matmul_samples[MATMUL_REPS];


void matmul_run(void)
{
    metric_reset(MATMUL_SLOT);

    heap_init();

    int32_t* mat_a   = (int32_t*)kMalloc(MATMUL_DIM * MATMUL_DIM * sizeof(int32_t));
    int32_t* mat_b   = (int32_t*)kMalloc(MATMUL_DIM * MATMUL_DIM * sizeof(int32_t));
    int32_t* mat_out = (int32_t*)kMalloc(MATMUL_DIM * MATMUL_DIM * sizeof(int32_t));

    for (int i = 0; i < MATMUL_DIM * MATMUL_DIM; i++)
    {
        mat_a[i] = (int32_t)(i * 3 + 1);
        mat_b[i] = (int32_t)(i - 7);
    }

    uint32_t checksum = 0u;

    for (int rep_index = 0; rep_index < MATMUL_REPS; rep_index++)
    {
        MEASURE_BEGIN(MATMUL_SLOT);

        for (int row = 0; row < MATMUL_DIM; row++)
        {
            for (int col = 0; col < MATMUL_DIM; col++)
            {
                int32_t acc = 0;

                for (int k = 0; k < MATMUL_DIM; k++)
                {
                    acc += mat_a[row * MATMUL_DIM + k] * mat_b[k * MATMUL_DIM + col];
                }

                mat_out[row * MATMUL_DIM + col] = acc;
            }
        }

        MEASURE_END_INTO(MATMUL_SLOT, g_matmul_samples[rep_index]);

        for (int element_index = 0; element_index < MATMUL_DIM * MATMUL_DIM; element_index++)
        {
            checksum += (uint32_t)mat_out[element_index];
        }
    }

    g_matmul_checksum = checksum;

    kFree(mat_a);
    kFree(mat_b);
    kFree(mat_out);

    heap_destroy();
}
