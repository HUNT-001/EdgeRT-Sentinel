// Vitis HLS top level: evaluates a batch of risk samples with one configuration.
// The pipeline target is II=1; the achieved II is whatever csynth reports.
//
// History: v1 passed in[i] straight to risk_eval -> one AXI read per struct field -> II=5.
// v2 copied the record locally but the 16-byte struct still needed two 64-bit beats -> II=2
// (the `aggregate` pragma is not allowed on m_axi ports). v3 (this file) moves each record as one
// 128-bit input word and one 256-bit output word, so each iteration is one read and one write.
#include "risk_kernel.h"

using namespace edgert;

void risk_kernel(const InWord in[RISK_MAX_SAMPLES], OutWord out[RISK_MAX_SAMPLES], RiskConfig cfg, int count) {
#pragma HLS INTERFACE m_axi port = in offset = slave bundle = gmem0 depth = 256
#pragma HLS INTERFACE m_axi port = out offset = slave bundle = gmem1 depth = 256
#pragma HLS INTERFACE s_axilite port = cfg
#pragma HLS INTERFACE s_axilite port = count
#pragma HLS INTERFACE s_axilite port = return

    int n = count;
    if (n < 0) n = 0;
    if (n > RISK_MAX_SAMPLES) n = RISK_MAX_SAMPLES;

risk_loop:
    for (int i = 0; i < n; ++i) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 256
        const RiskInput x = unpack_input(in[i]);
        out[i] = pack_output(risk_eval(x, cfg));
    }
}
