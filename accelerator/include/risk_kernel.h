// Batch wrapper around risk_eval. This is the Vitis HLS top-level function.
#pragma once

#include "risk_types.h"

void risk_kernel(const edgert::RiskInput in[edgert::RISK_MAX_SAMPLES],
                 edgert::RiskOutput out[edgert::RISK_MAX_SAMPLES],
                 edgert::RiskConfig cfg, int count);
