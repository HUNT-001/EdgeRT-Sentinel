// Bus word format of the RACE kernel: one record per AXI beat so the loop can run at II=1.
//   input  = 128-bit word, output = 256-bit word, little-endian bit layout (LSB first):
//     in : dist[0:32) closing[32:64) ego[64:96) age[96:112) conf[112:120), rest zero
//     out: q15[0:16) level[16:32) ttc[32:64) stop[64:96) vmax[96:128) status[128:144), rest zero
// Under Vitis HLS the words are ap_uint. On a plain host compiler (no Vitis headers) they are byte
// arrays with the identical layout, so the same testbench and pack/unpack code run in both places.
#pragma once

#include <stdint.h>
#include <string.h>

#include "risk_types.h"

#if defined(__SYNTHESIS__) || (defined(__has_include) && __has_include(<ap_int.h>))
#include <ap_int.h>
namespace edgert {
typedef ap_uint<128> InWord;
typedef ap_uint<256> OutWord;
inline void clear_word(InWord& w) { w = 0; }
inline void clear_word(OutWord& w) { w = 0; }
template <int LO, int W, class T>
inline void put_bits(T& w, uint64_t v) { w.range(LO + W - 1, LO) = v; }
template <int LO, int W, class T>
inline uint64_t get_bits(const T& w) { return w.range(LO + W - 1, LO).to_uint64(); }
}  // namespace edgert
#else
namespace edgert {
struct InWord { uint8_t b[16]; };
struct OutWord { uint8_t b[32]; };
inline void clear_word(InWord& w) { memset(w.b, 0, sizeof w.b); }
inline void clear_word(OutWord& w) { memset(w.b, 0, sizeof w.b); }
template <int LO, int W, class T>
inline void put_bits(T& w, uint64_t v) {
    for (int i = 0; i < W; ++i) {
        const int p = LO + i;
        if ((v >> i) & 1) w.b[p >> 3] = static_cast<uint8_t>(w.b[p >> 3] | (1u << (p & 7)));
        else w.b[p >> 3] = static_cast<uint8_t>(w.b[p >> 3] & ~(1u << (p & 7)));
    }
}
template <int LO, int W, class T>
inline uint64_t get_bits(const T& w) {
    uint64_t v = 0;
    for (int i = 0; i < W; ++i) {
        const int p = LO + i;
        v |= static_cast<uint64_t>((w.b[p >> 3] >> (p & 7)) & 1u) << i;
    }
    return v;
}
}  // namespace edgert
#endif

namespace edgert {

inline InWord pack_input(const RiskInput& x) {
    InWord w;
    clear_word(w);
    put_bits<0, 32>(w, x.obstacle_distance_mm);
    put_bits<32, 32>(w, static_cast<uint32_t>(x.closing_speed_mm_s));
    put_bits<64, 32>(w, x.ego_speed_mm_s);
    put_bits<96, 16>(w, x.sensor_age_ms);
    put_bits<112, 8>(w, x.confidence_u8);
    return w;
}

inline RiskInput unpack_input(const InWord& w) {
    RiskInput x;
    x.obstacle_distance_mm = static_cast<uint32_t>(get_bits<0, 32>(w));
    x.closing_speed_mm_s = static_cast<int32_t>(static_cast<uint32_t>(get_bits<32, 32>(w)));
    x.ego_speed_mm_s = static_cast<uint32_t>(get_bits<64, 32>(w));
    x.sensor_age_ms = static_cast<uint16_t>(get_bits<96, 16>(w));
    x.confidence_u8 = static_cast<uint8_t>(get_bits<112, 8>(w));
    return x;
}

inline OutWord pack_output(const RiskOutput& o) {
    OutWord w;
    clear_word(w);
    put_bits<0, 16>(w, o.risk_q15);
    put_bits<16, 16>(w, o.risk_level);
    put_bits<32, 32>(w, o.time_to_collision_ms);
    put_bits<64, 32>(w, o.stopping_distance_mm);
    put_bits<96, 32>(w, o.max_safe_speed_mm_s);
    put_bits<128, 16>(w, o.status);
    return w;
}

inline RiskOutput unpack_output(const OutWord& w) {
    RiskOutput o;
    o.risk_q15 = static_cast<uint16_t>(get_bits<0, 16>(w));
    o.risk_level = static_cast<uint16_t>(get_bits<16, 16>(w));
    o.time_to_collision_ms = static_cast<uint32_t>(get_bits<32, 32>(w));
    o.stopping_distance_mm = static_cast<uint32_t>(get_bits<64, 32>(w));
    o.max_safe_speed_mm_s = static_cast<uint32_t>(get_bits<96, 32>(w));
    o.status = static_cast<uint16_t>(get_bits<128, 16>(w));
    return o;
}

}  // namespace edgert
