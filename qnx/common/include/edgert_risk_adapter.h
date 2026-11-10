// Conversions between wire messages and the RACE core types.
#pragma once

#include <string.h>

#include "edgert_protocol.h"
#include "risk_types.h"

namespace edgert {

inline RiskInput to_risk_input(const SensorFrame& s) {
    RiskInput in;
    in.obstacle_distance_mm = s.obstacle_distance_mm;
    in.closing_speed_mm_s = s.closing_speed_mm_s;
    in.ego_speed_mm_s = s.ego_speed_mm_s;
    in.sensor_age_ms = s.sensor_age_ms;
    // A frame the sensor itself flagged invalid must not be trusted: force it past the input range.
    if (s.flags & SENSOR_FLAG_INVALID) in.obstacle_distance_mm = RISK_DIST_MAX_MM + 1;
    in.confidence_u8 = s.confidence_u8;
    return in;
}

inline RiskResponse make_risk_response(uint32_t sequence, uint64_t now_ns, const RiskOutput& o) {
    RiskResponse r;
    memset(&r, 0, sizeof r);
    r.header = make_header(MsgType::RISK_RESPONSE);
    r.sequence = sequence;
    r.computed_timestamp_ns = now_ns;
    r.risk_q15 = o.risk_q15;
    r.risk_level = o.risk_level;
    r.time_to_collision_ms = o.time_to_collision_ms;
    r.stopping_distance_mm = o.stopping_distance_mm;
    r.max_safe_speed_mm_s = o.max_safe_speed_mm_s;
    r.status = o.status;
    return r;
}

// A response is acceptable to the controller only if it is well-formed, matches the
// outstanding request, and the core marked it valid.
inline bool risk_response_usable(const RiskResponse& r, uint32_t expected_sequence) {
    return r.header.type == static_cast<uint16_t>(MsgType::RISK_RESPONSE) && r.header.version == PROTOCOL_VERSION &&
           r.sequence == expected_sequence && r.status == RISK_VALID;
}

}  // namespace edgert
