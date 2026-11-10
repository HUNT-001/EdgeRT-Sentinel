// Control policy and independent safety monitor: pure logic, no OS calls, fully deterministic.
// motiond calls Controller::step() once per control cycle; safetyd calls SafetyMonitor.
// See docs/safety_policy.md for the rationale behind every number here.
#pragma once

#include <stdint.h>

#include "edgert_protocol.h"
#include "risk_types.h"

namespace edgert {

struct ControllerConfig {
    uint32_t cycle_ms = 20;                 // 50 Hz
    uint32_t nominal_speed_mm_s = 1000;
    uint32_t degraded_speed_cap_mm_s = 250; // never faster than this while degraded
    uint32_t max_input_age_ms = 100;        // sensor age + transport age above this = unhealthy
    uint32_t unhealthy_cycles_to_stop = 10; // 200 ms of bad input -> SAFE_STOP
    uint32_t healthy_cycles_to_recover = 25;// 500 ms of good input before leaving DEGRADED
    uint32_t critical_cycles_to_stop = 5;   // 100 ms sustained CRITICAL risk -> SAFE_STOP
    uint32_t ramp_up_mm_s_per_cycle = 10;   // 500 mm/s^2; slowing down is never rate-limited
};

struct CycleInput {
    bool risk_valid;          // a usable RiskResponse for the freshest sensor frame (risk_response_usable)
    uint16_t risk_level;      // RiskLevel, meaningful only if risk_valid
    uint32_t max_safe_speed_mm_s;
    uint32_t input_age_ms;    // sensor_age_ms + time since the frame was received locally
    bool external_stop;       // safetyd demanded SAFE_STOP
    SafetyReason external_reason;
    bool reset_request;       // explicit operator action
};

struct CycleOutput {
    SafetyState state;
    SafetyReason reason;      // why the state is not NORMAL (NONE when NORMAL)
    uint32_t speed_mm_s;      // commanded speed, both wheels
    bool transitioned;
};

class Controller {
public:
    explicit Controller(const ControllerConfig& c = ControllerConfig()) : c_(c) {}

    CycleOutput step(const CycleInput& in) {
        const SafetyState before = state_;

        if (in.external_stop && state_ != SafetyState::SAFE_STOP) enter_stop(in.external_reason);

        const bool fresh = in.input_age_ms <= c_.max_input_age_ms;
        const bool healthy = in.risk_valid && fresh;

        if (state_ == SafetyState::SAFE_STOP) {
            // Latched. Leaves only by explicit reset while inputs are healthy and not critical.
            if (in.reset_request && !in.external_stop && healthy && in.risk_level != RISK_LEVEL_CRITICAL) {
                state_ = SafetyState::DEGRADED;
                reason_ = SafetyReason::NONE;
                unhealthy_ = critical_ = healthy_ = 0;
            }
        } else if (!healthy) {
            healthy_ = 0;
            critical_ = 0;
            ++unhealthy_;
            reason_ = !in.risk_valid ? SafetyReason::RISK_INVALID : SafetyReason::SENSOR_STALE;
            if (unhealthy_ >= c_.unhealthy_cycles_to_stop) enter_stop(reason_);
            else state_ = SafetyState::DEGRADED;
        } else {
            unhealthy_ = 0;
            ++healthy_;
            critical_ = (in.risk_level == RISK_LEVEL_CRITICAL) ? critical_ + 1 : 0;
            if (critical_ >= c_.critical_cycles_to_stop) {
                enter_stop(SafetyReason::CRITICAL_RISK);
            } else if (state_ == SafetyState::DEGRADED && healthy_ >= c_.healthy_cycles_to_recover) {
                state_ = SafetyState::NORMAL;
                reason_ = SafetyReason::NONE;
            }
        }

        uint32_t target = 0;
        if (state_ == SafetyState::NORMAL) {
            target = min(c_.nominal_speed_mm_s, in.max_safe_speed_mm_s);
        } else if (state_ == SafetyState::DEGRADED) {
            target = min(c_.degraded_speed_cap_mm_s, c_.nominal_speed_mm_s);
            if (in.risk_valid) target = min(target, in.max_safe_speed_mm_s);
        }
        if (target <= speed_) speed_ = target;                       // slow down immediately
        else speed_ = min(target, speed_ + c_.ramp_up_mm_s_per_cycle);  // speed up gradually

        return CycleOutput{state_, reason_, speed_, state_ != before};
    }

    SafetyState state() const { return state_; }

private:
    static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }
    void enter_stop(SafetyReason r) {
        state_ = SafetyState::SAFE_STOP;
        reason_ = r;
    }

    ControllerConfig c_;
    SafetyState state_ = SafetyState::DEGRADED;  // power-up is DEGRADED until inputs prove healthy
    SafetyReason reason_ = SafetyReason::NONE;
    uint32_t speed_ = 0, unhealthy_ = 0, healthy_ = 0, critical_ = 0;
};

// Independent watchdog on motiond's heartbeat. Knows nothing about risk or sensors, so a bug in
// motiond's control path cannot disable it. Latches SAFE_STOP; cleared only by reset().
struct MonitorConfig {
    uint32_t heartbeat_timeout_ms = 60;  // 3 control cycles
    uint32_t startup_grace_ms = 500;     // time allowed for the first heartbeat after safetyd starts
};

class SafetyMonitor {
public:
    explicit SafetyMonitor(uint64_t start_ns, const MonitorConfig& c = MonitorConfig()) : c_(c), start_ns_(start_ns) {}

    // Heartbeat value is motiond's cycle counter; it must advance every beat.
    void heartbeat(uint64_t now_ns, uint32_t cycle) {
        if (seen_ && cycle == last_cycle_) {  // frozen counter = control loop wedged while pulses still fire
            trip_ = true;
        }
        seen_ = true;
        last_cycle_ = cycle;
        last_ns_ = now_ns;
    }

    // Returns true while a SAFE_STOP must be asserted.
    bool evaluate(uint64_t now_ns) {
        if (trip_) return true;
        const uint64_t ref = seen_ ? last_ns_ : start_ns_;
        const uint64_t limit_ms = seen_ ? c_.heartbeat_timeout_ms : c_.startup_grace_ms;
        if (now_ns > ref && (now_ns - ref) / 1000000ULL > limit_ms) trip_ = true;
        return trip_;
    }

    void reset(uint64_t now_ns) {
        trip_ = false;
        seen_ = false;
        start_ns_ = now_ns;
    }

    bool tripped() const { return trip_; }

private:
    MonitorConfig c_;
    uint64_t start_ns_, last_ns_ = 0;
    uint32_t last_cycle_ = 0;
    bool seen_ = false, trip_ = false;
};

}  // namespace edgert
