// Gate 1 HLS smoke kernel: batched Q15 x Q15 -> Q15 multiply with
// round-half-up and saturation. Same integer style the RACE risk core will use.
// Plain <cstdint> types so the identical source builds with g++ and Vitis HLS.
#pragma once
#include <cstdint>

constexpr int SMOKE_MAX = 256;

void smoke_kernel(const int16_t a[SMOKE_MAX], const int16_t b[SMOKE_MAX], int16_t out[SMOKE_MAX], int count);
