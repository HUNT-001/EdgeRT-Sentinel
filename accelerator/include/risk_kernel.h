// Batch wrapper around risk_eval. This is the Vitis HLS top-level function.
// Records cross the AXI bus as single wide words (see risk_wire.h), not as C structs.
#pragma once

#include "risk_types.h"
#include "risk_wire.h"

void risk_kernel(const edgert::InWord in[edgert::RISK_MAX_SAMPLES], edgert::OutWord out[edgert::RISK_MAX_SAMPLES],
                 edgert::RiskConfig cfg, int count);
