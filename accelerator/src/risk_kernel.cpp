// Vitis HLS top level: evaluates a batch of risk samples with one configuration.
// The pipeline target is II=1; the achieved II is whatever csynth reports.
//
// Bus access: each record is copied into a local variable ONCE and each result is written ONCE.
// Passing in[i] straight to risk_eval made HLS issue one AXI read per field (5 reads through one
// port -> II=5); the aggregate pragmas pack each struct into a single wide bus word.
#include "risk_kernel.h"

#ifndef RISK_HLS_VARIANT
#define RISK_HLS_VARIANT 0
#endif

using namespace edgert;

void risk_kernel(const RiskInput in[RISK_MAX_SAMPLES], RiskOutput out[RISK_MAX_SAMPLES], RiskConfig cfg, int count) {
// RISK_HLS_VARIANT selects the bus-packing experiment (see accelerator/hls/sweep_interface.bat):
//   0 aggregate auto, default m_axi   1 + max_widen_bitwidth=128   2 aggregate compact=bit   3 aggregate compact=byte
#if RISK_HLS_VARIANT == 1
#pragma HLS INTERFACE m_axi port = in offset = slave bundle = gmem0 depth = 256 max_widen_bitwidth = 128
#pragma HLS INTERFACE m_axi port = out offset = slave bundle = gmem1 depth = 256 max_widen_bitwidth = 256
#else
#pragma HLS INTERFACE m_axi port = in offset = slave bundle = gmem0 depth = 256
#pragma HLS INTERFACE m_axi port = out offset = slave bundle = gmem1 depth = 256
#endif
#if RISK_HLS_VARIANT == 2
#pragma HLS aggregate variable = in compact = bit
#pragma HLS aggregate variable = out compact = bit
#elif RISK_HLS_VARIANT == 3
#pragma HLS aggregate variable = in compact = byte
#pragma HLS aggregate variable = out compact = byte
#else
#pragma HLS aggregate variable = in compact = auto
#pragma HLS aggregate variable = out compact = auto
#endif
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
