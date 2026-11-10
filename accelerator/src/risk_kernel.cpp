// Vitis HLS top level: evaluates a batch of risk samples with one configuration.
// The pipeline target is II=1; the achieved II is whatever csynth reports.
//
// Bus access: each record is copied into a local variable ONCE and each result is written ONCE.
// Passing in[i] straight to risk_eval made HLS issue one AXI read per field (5 reads through one
// port -> II=5); the aggregate pragmas pack each struct into a single wide bus word.
#include "risk_kernel.h"

using namespace edgert;

void risk_kernel(const RiskInput in[RISK_MAX_SAMPLES], RiskOutput out[RISK_MAX_SAMPLES], RiskConfig cfg, int count) {
#pragma HLS INTERFACE m_axi port = in offset = slave bundle = gmem0 depth = 256
#pragma HLS INTERFACE m_axi port = out offset = slave bundle = gmem1 depth = 256
#pragma HLS aggregate variable = in compact = auto
#pragma HLS aggregate variable = out compact = auto
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
        const RiskInput x = in[i];             // one wide read
        const RiskOutput o = risk_eval(x, cfg);
        out[i] = o;                            // one wide write
    }
}
