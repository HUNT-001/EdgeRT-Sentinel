// RACE risk core. Implements docs/risk_spec.md exactly.
#include "risk_types.h"

namespace edgert {

namespace {

// Floor square root, digit by digit. Fixed 32 iterations: HLS-friendly.
uint32_t isqrt64(uint64_t n) {
    uint64_t res = 0;
    uint64_t bit = 1ULL << 62;
    for (int i = 0; i < 32; ++i) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<uint32_t>(res);
}

RiskOutput fail_safe(uint16_t status, uint32_t speed) {
    RiskOutput o;
    o.risk_q15 = RISK_Q15_MAX;
    o.risk_level = RISK_LEVEL_INVALID;
    o.time_to_collision_ms = 0;
    o.stopping_distance_mm = 0;
    o.max_safe_speed_mm_s = speed;
    o.status = status;
    return o;
}

bool config_ok(const RiskConfig& c) {
    return c.decel_mm_s2 >= 1 && c.decel_mm_s2 <= RISK_DECEL_MAX_MM_S2 &&
           c.t_warn_ms > c.t_crit_ms && c.level_medium_q15 < c.level_high_q15 &&
           c.level_high_q15 < c.level_critical_q15 && c.level_critical_q15 <= RISK_Q15_MAX;
}

}  // namespace

RiskConfig default_risk_config() {
    RiskConfig c;
    c.decel_mm_s2 = 1500;
    c.reaction_time_ms = 100;
    c.margin_mm = 300;
    c.t_warn_ms = 3000;
    c.t_crit_ms = 1000;
    c.min_confidence_u8 = 128;
    c.max_sensor_age_ms = 100;
    c.level_medium_q15 = 8192;
    c.level_high_q15 = 20000;
    c.level_critical_q15 = 28000;
    c.v_max_mm_s = 1500;
    c.v_restricted_mm_s = 700;
    c.v_crawl_mm_s = 250;
    c.v_invalid_mm_s = 200;
    return c;
}

RiskOutput risk_eval(const RiskInput& in, const RiskConfig& c) {
    if (!config_ok(c)) return fail_safe(RISK_COMPUTATION_ERROR, 0);

    if (in.obstacle_distance_mm > RISK_DIST_MAX_MM || in.ego_speed_mm_s > static_cast<uint32_t>(RISK_SPEED_MAX_MM_S) ||
        in.closing_speed_mm_s > RISK_SPEED_MAX_MM_S || in.closing_speed_mm_s < -RISK_SPEED_MAX_MM_S)
        return fail_safe(RISK_INVALID_INPUT, c.v_invalid_mm_s);
    if (in.sensor_age_ms > c.max_sensor_age_ms) return fail_safe(RISK_STALE_SENSOR, c.v_invalid_mm_s);
    if (in.confidence_u8 < c.min_confidence_u8) return fail_safe(RISK_LOW_CONFIDENCE, c.v_invalid_mm_s);

    const uint32_t d = in.obstacle_distance_mm;
    const uint32_t ve = in.ego_speed_mm_s;
    const uint32_t a = c.decel_mm_s2;
    const uint32_t tr = c.reaction_time_ms;
    const uint32_t margin = c.margin_mm;
    const uint32_t t_warn = c.t_warn_ms;
    const uint32_t t_crit = c.t_crit_ms;
    const bool closing = in.closing_speed_mm_s > 0;

    uint32_t ttc = RISK_TTC_MAX_MS;
    if (closing) {
        ttc = (1000u * d) / static_cast<uint32_t>(in.closing_speed_mm_s);
        if (ttc > RISK_TTC_MAX_MS) ttc = RISK_TTC_MAX_MS;
    }

    const uint32_t d_stop = (ve * ve) / (2u * a);
    const uint32_t d_rea = (ve * tr) / 1000u;
    const uint32_t stopping = d_stop + d_rea;
    const uint32_t d_req = stopping + margin;

    uint32_t r_d = 0;
    if (d_req > d) {
        const uint64_t q = (static_cast<uint64_t>(d_req - d) << 15) / d_req;
        r_d = q > RISK_Q15_MAX ? RISK_Q15_MAX : static_cast<uint32_t>(q);
    }
    uint32_t r_t = 0;
    if (closing && ttc < t_warn) {
        const uint32_t q = ((t_warn - ttc) << 15) / (t_warn - t_crit);
        r_t = q > RISK_Q15_MAX ? RISK_Q15_MAX : q;
    }
    const uint32_t risk = r_d > r_t ? r_d : r_t;

    uint16_t level;
    uint32_t limit;
    if (risk >= c.level_critical_q15) {
        level = RISK_LEVEL_CRITICAL;
        limit = 0;
    } else if (risk >= c.level_high_q15) {
        level = RISK_LEVEL_HIGH;
        limit = c.v_crawl_mm_s;
    } else if (risk >= c.level_medium_q15) {
        level = RISK_LEVEL_MEDIUM;
        limit = c.v_restricted_mm_s;
    } else {
        level = RISK_LEVEL_LOW;
        limit = c.v_max_mm_s;
    }

    const uint32_t m = d > margin ? d - margin : 0;
    uint32_t v_dist = 0;
    if (m > 0) {
        const uint32_t b = (a * tr + 999u) / 1000u;
        const uint64_t s = static_cast<uint64_t>(b) * b + static_cast<uint64_t>(2u) * a * m;
        v_dist = isqrt64(s) - b;
    }

    RiskOutput o;
    o.risk_q15 = static_cast<uint16_t>(risk);
    o.risk_level = level;
    o.time_to_collision_ms = ttc;
    o.stopping_distance_mm = stopping;
    o.max_safe_speed_mm_s = limit < v_dist ? limit : v_dist;
    o.status = RISK_VALID;
    return o;
}

}  // namespace edgert
