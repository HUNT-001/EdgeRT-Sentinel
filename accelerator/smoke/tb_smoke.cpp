// Self-checking testbench. Golden model is written independently of the kernel
// (int64 + explicit floor division, no shifts). Returns 0 on pass, as Vitis
// csim/cosim require.
#include <cstdint>
#include <cstdio>

#include "smoke_kernel.h"

static int16_t golden(int16_t a, int16_t b) {
    int64_t num = static_cast<int64_t>(a) * b + 16384;
    int64_t q = num >= 0 ? num / 32768 : -((-num + 32767) / 32768);  // floor(num / 2^15)
    if (q > 32767) q = 32767;
    if (q < -32768) q = -32768;
    return static_cast<int16_t>(q);
}

static uint32_t lcg(uint32_t& s) { return s = s * 1664525u + 1013904223u; }

int main() {
    static int16_t a[SMOKE_MAX], b[SMOKE_MAX], out[SMOKE_MAX];
    const int16_t edges[] = {-32768, -32767, -16384, -1, 0, 1, 16384, 32767};
    int n = 0;
    for (int16_t x : edges)
        for (int16_t y : edges) { a[n] = x; b[n] = y; ++n; }  // 64 directed corner cases
    uint32_t seed = 0xED6E5EEDu;
    for (; n < SMOKE_MAX; ++n) {
        a[n] = static_cast<int16_t>(lcg(seed) >> 16);
        b[n] = static_cast<int16_t>(lcg(seed) >> 16);
    }

    int errors = 0;
    const int counts[] = {SMOKE_MAX, 37};  // full batch and a partial batch
    for (int count : counts) {
        for (int i = 0; i < SMOKE_MAX; ++i) out[i] = 0x5A5A;
        smoke_kernel(a, b, out, count);
        for (int i = 0; i < SMOKE_MAX; ++i) {
            const int16_t want = i < count ? golden(a[i], b[i]) : static_cast<int16_t>(0x5A5A);
            if (out[i] != want) {
                if (errors < 10)
                    std::printf("MISMATCH count=%d i=%d a=%d b=%d got=%d want=%d\n", count, i, a[i], b[i], out[i], want);
                ++errors;
            }
        }
    }
    std::printf("GATE1 HLS SMOKE TB: %s (%d errors)\n", errors ? "FAIL" : "PASS", errors);
    return errors ? 1 : 0;
}
