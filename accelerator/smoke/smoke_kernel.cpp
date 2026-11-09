#include "smoke_kernel.h"

static int16_t sat_q15(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

void smoke_kernel(const int16_t a[SMOKE_MAX], const int16_t b[SMOKE_MAX], int16_t out[SMOKE_MAX], int count) {
#pragma HLS INTERFACE m_axi port=a   offset=slave bundle=gmem0 depth=256
#pragma HLS INTERFACE m_axi port=b   offset=slave bundle=gmem1 depth=256
#pragma HLS INTERFACE m_axi port=out offset=slave bundle=gmem0 depth=256
#pragma HLS INTERFACE s_axilite port=count
#pragma HLS INTERFACE s_axilite port=return

    if (count < 0) count = 0;
    if (count > SMOKE_MAX) count = SMOKE_MAX;

smoke_loop:
    for (int i = 0; i < count; ++i) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=256
        const int32_t prod = static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);  // Q30
        // Round half up, then arithmetic shift back to Q15 (two's complement).
        out[i] = sat_q15((prod + (1 << 14)) >> 15);
    }
}
