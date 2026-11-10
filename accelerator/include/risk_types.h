// RACE risk engine: shared types. See docs/risk_spec.md for the contract.
// Portable C++: no STL, no floating point, no heap. The same source is used by
// the QNX riskd service and by Vitis HLS.
#pragma once

#include <stdint.h>

namespace edgert {

constexpr uint32_t RISK_DIST_MAX_MM = 200000;
constexpr int32_t RISK_SPEED_MAX_MM_S = 20000;
constexpr uint32_t RISK_DECEL_MAX_MM_S2 = 20000;
constexpr uint32_t RISK_TTC_MAX_MS = 60000;
constexpr uint16_t RISK_Q15_MAX = 32767;
constexpr int RISK_MAX_SAMPLES = 256;

enum RiskStatus : uint16_t {
    RISK_VALID = 0,
    RISK_STALE_SENSOR = 1,
    RISK_LOW_CONFIDENCE = 2,
    RISK_INVALID_INPUT = 3,
    RISK_COMPUTATION_ERROR = 4
};

enum RiskLevel : uint16_t {
    RISK_LEVEL_LOW = 0,
    RISK_LEVEL_MEDIUM = 1,
    RISK_LEVEL_HIGH = 2,
    RISK_LEVEL_CRITICAL = 3,
    RISK_LEVEL_INVALID = 4
};

struct RiskInput {
    uint32_t obstacle_distance_mm;
    int32_t closing_speed_mm_s;
    uint32_t ego_speed_mm_s;
    uint16_t sensor_age_ms;
    uint8_t confidence_u8;
};

struct RiskConfig {
    uint32_t decel_mm_s2;
    uint16_t reaction_time_ms;
    uint16_t margin_mm;
    uint16_t t_warn_ms;
    uint16_t t_crit_ms;
    uint8_t min_confidence_u8;
    uint16_t max_sensor_age_ms;
    uint16_t level_medium_q15;
    uint16_t level_high_q15;
    uint16_t level_critical_q15;
    uint32_t v_max_mm_s;
    uint32_t v_restricted_mm_s;
    uint32_t v_crawl_mm_s;
    uint32_t v_invalid_mm_s;
};

struct RiskOutput {
    uint16_t risk_q15;
    uint16_t risk_level;
    uint32_t time_to_collision_ms;
    uint32_t stopping_distance_mm;
    uint32_t max_safe_speed_mm_s;
    uint16_t status;
};

RiskConfig default_risk_config();
RiskOutput risk_eval(const RiskInput& in, const RiskConfig& cfg);

}  // namespace edgert
