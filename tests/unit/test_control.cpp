// Policy tests: exact transition timing, latching, ramping, watchdog, and scenario replays
// that check safety invariants on every cycle.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "edgert_control.h"

using namespace edgert;

static int failures = 0, checks = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        ++checks;                                                 \
        if (!(c)) {                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
            ++failures;                                           \
        }                                                         \
    } while (0)

static CycleInput good(uint32_t vmax = 1500, uint16_t level = RISK_LEVEL_LOW) {
    return CycleInput{true, level, vmax, 10, false, SafetyReason::NONE, false};
}
static CycleInput stale() { CycleInput i = good(); i.input_age_ms = 500; return i; }
static CycleInput invalid() { CycleInput i = good(); i.risk_valid = false; return i; }

static void run(Controller& c, const CycleInput& in, int n, CycleOutput* last = nullptr) {
    for (int i = 0; i < n; ++i) { CycleOutput o = c.step(in); if (last) *last = o; }
}

static void test_startup_and_ramp() {
    Controller c;
    CycleOutput o = c.step(good());
    CHECK(o.state == SafetyState::DEGRADED && o.speed_mm_s == 10);  // capped + ramped
    run(c, good(), 30, &o);
    CHECK(o.state == SafetyState::NORMAL);
    CHECK(o.speed_mm_s > 250 && o.speed_mm_s <= 1000);
    run(c, good(), 200, &o);
    CHECK(o.speed_mm_s == 1000);  // nominal, below max_safe 1500
    // Slowing is immediate, not ramped.
    o = c.step(good(300));
    CHECK(o.speed_mm_s == 300);
    // Speeding up is ramped.
    o = c.step(good(1500));
    CHECK(o.speed_mm_s == 310);
}

static void test_recovery_hysteresis_exact() {
    Controller c;
    CycleOutput o;
    for (int i = 1; i <= 24; ++i) { o = c.step(good()); CHECK(o.state == SafetyState::DEGRADED); }
    o = c.step(good());  // 25th healthy cycle
    CHECK(o.state == SafetyState::NORMAL && o.transitioned);
}

static void test_unhealthy_to_stop_exact() {
    Controller c;
    CycleOutput o;
    run(c, good(), 40, &o);
    CHECK(o.state == SafetyState::NORMAL);
    for (int i = 1; i <= 9; ++i) {
        o = c.step(stale());
        CHECK(o.state == SafetyState::DEGRADED && o.reason == SafetyReason::SENSOR_STALE);
        CHECK(o.speed_mm_s <= 250);
    }
    o = c.step(stale());  // 10th = 200 ms
    CHECK(o.state == SafetyState::SAFE_STOP && o.speed_mm_s == 0 && o.reason == SafetyReason::SENSOR_STALE);
    // Latched: healthy inputs alone never release it.
    run(c, good(), 500, &o);
    CHECK(o.state == SafetyState::SAFE_STOP && o.speed_mm_s == 0);
}

static void test_blip_recovers_without_stop() {
    Controller c;
    CycleOutput o;
    run(c, good(), 40, &o);
    run(c, invalid(), 5, &o);
    CHECK(o.state == SafetyState::DEGRADED && o.reason == SafetyReason::RISK_INVALID);
    run(c, good(), 25, &o);
    CHECK(o.state == SafetyState::NORMAL && o.reason == SafetyReason::NONE);
    // A blip resets the unhealthy count; two 9-cycle gaps separated by one good cycle never stop us.
    run(c, stale(), 9, &o);
    run(c, good(), 1, &o);
    run(c, stale(), 9, &o);
    CHECK(o.state == SafetyState::DEGRADED);
}

static void test_age_boundary() {
    Controller c;
    CycleOutput o;
    run(c, good(), 40, &o);
    CycleInput i = good(); i.input_age_ms = 100;   // exactly the limit: healthy
    run(c, i, 30, &o);
    CHECK(o.state == SafetyState::NORMAL);
    i.input_age_ms = 101;                          // one over: unhealthy
    o = c.step(i);
    CHECK(o.state == SafetyState::DEGRADED && o.reason == SafetyReason::SENSOR_STALE);
}

static void test_degraded_obeys_max_safe() {
    Controller c;
    CycleOutput o;
    run(c, good(), 5, &o);                          // still DEGRADED, speed ramping
    CHECK(o.state == SafetyState::DEGRADED);
    o = c.step(good(5));                            // valid risk says 5 mm/s: tighter than the 250 cap
    CHECK(o.speed_mm_s == 5);
}

static void test_unhealthy_breaks_critical_streak() {
    Controller c;
    CycleOutput o;
    run(c, good(), 40, &o);
    run(c, good(200, RISK_LEVEL_CRITICAL), 4, &o);
    run(c, invalid(), 1, &o);                       // streak must restart after any unhealthy cycle
    run(c, good(200, RISK_LEVEL_CRITICAL), 4, &o);
    CHECK(o.state != SafetyState::SAFE_STOP);
}

static void test_critical_sustained() {
    Controller c;
    CycleOutput o;
    run(c, good(), 40, &o);
    run(c, good(200, RISK_LEVEL_CRITICAL), 4, &o);
    CHECK(o.state != SafetyState::SAFE_STOP);
    run(c, good(1500, RISK_LEVEL_LOW), 1, &o);       // critical streak broken
    run(c, good(200, RISK_LEVEL_CRITICAL), 4, &o);
    CHECK(o.state != SafetyState::SAFE_STOP);
    o = c.step(good(200, RISK_LEVEL_CRITICAL));      // 5th consecutive
    CHECK(o.state == SafetyState::SAFE_STOP && o.reason == SafetyReason::CRITICAL_RISK && o.speed_mm_s == 0);
}

static void test_reset_rules() {
    Controller c;
    CycleOutput o;
    run(c, stale(), 12, &o);
    CHECK(o.state == SafetyState::SAFE_STOP);
    CycleInput r = stale(); r.reset_request = true;
    o = c.step(r);                                       // reset while unhealthy: refused
    CHECK(o.state == SafetyState::SAFE_STOP);
    r = good(200, RISK_LEVEL_CRITICAL); r.reset_request = true;
    o = c.step(r);                                       // reset while critical: refused
    CHECK(o.state == SafetyState::SAFE_STOP);
    r = good(); r.reset_request = true; r.external_stop = true; r.external_reason = SafetyReason::HEARTBEAT_LOST;
    o = c.step(r);                                       // reset while safetyd still demands stop: refused
    CHECK(o.state == SafetyState::SAFE_STOP);
    r = good(); r.reset_request = true;
    o = c.step(r);                                       // accepted: back through DEGRADED, speed ramps from 0
    CHECK(o.state == SafetyState::DEGRADED && o.speed_mm_s <= 10);
}

static void test_external_stop_overrides() {
    Controller c;
    CycleOutput o;
    run(c, good(), 60, &o);
    CycleInput e = good(); e.external_stop = true; e.external_reason = SafetyReason::HEARTBEAT_LOST;
    o = c.step(e);
    CHECK(o.state == SafetyState::SAFE_STOP && o.speed_mm_s == 0 && o.reason == SafetyReason::HEARTBEAT_LOST);
}

static const uint64_t MS = 1000000ULL;

static void test_monitor() {
    {
        SafetyMonitor m(0);
        CHECK(!m.evaluate(400 * MS));   // within startup grace
        CHECK(m.evaluate(501 * MS));    // never heard from motiond
    }
    {
        SafetyMonitor m(0);
        uint64_t t = 0;
        for (uint32_t k = 1; k <= 100; ++k) { t += 20 * MS; m.heartbeat(t, k); CHECK(!m.evaluate(t)); }
        CHECK(!m.evaluate(t + 60 * MS));  // exactly at the limit: still ok
        CHECK(m.evaluate(t + 61 * MS));   // beyond: trip
        m.heartbeat(t + 80 * MS, 101);    // late heartbeat does not un-trip
        CHECK(m.tripped());
        m.reset(t + 100 * MS);
        CHECK(!m.tripped());
    }
    {
        SafetyMonitor m(0);                // pulses keep coming but the counter is frozen
        m.heartbeat(20 * MS, 7);
        m.heartbeat(40 * MS, 7);
        CHECK(m.evaluate(41 * MS));
    }
}

// ---- scenario replay with invariants ------------------------------------------------------------

struct Fr { long t_ms, flags, age, level, vmax, status; };

static bool load(const char* path, std::vector<Fr>& out) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return false; }
    while (fgets(line, sizeof line, f)) {
        long v[14]; char* p = line;
        for (int i = 0; i < 14; ++i) { char* e; v[i] = strtol(p, &e, 10); p = (*e == ',') ? e + 1 : e; }
        out.push_back(Fr{v[1], v[2], v[6], v[9], v[12], v[13]});
    }
    fclose(f);
    return !out.empty();
}

struct Result { SafetyState final_state; SafetyReason reason; uint32_t final_speed; long stop_t_ms; bool saw_degraded; bool violated; };

static Result replay(const std::vector<Fr>& fr, long duration_ms) {
    Controller c;
    Result r{SafetyState::DEGRADED, SafetyReason::NONE, 0, -1, false, false};
    size_t idx = 0;
    bool have = false;
    Fr cur{};
    for (long t = 0; t <= duration_ms; t += 20) {
        while (idx < fr.size() && fr[idx].t_ms <= t) { cur = fr[idx++]; have = true; }
        CycleInput in{};
        in.risk_valid = have && cur.status == RISK_VALID;
        in.risk_level = have ? static_cast<uint16_t>(cur.level) : static_cast<uint16_t>(RISK_LEVEL_INVALID);
        in.max_safe_speed_mm_s = have ? static_cast<uint32_t>(cur.vmax) : 0;
        in.input_age_ms = have ? static_cast<uint32_t>(cur.age + (t - cur.t_ms)) : 65535;
        CycleOutput o = c.step(in);
        // Invariants that must hold on every single cycle, whatever the scenario:
        if (o.state == SafetyState::SAFE_STOP && o.speed_mm_s != 0) r.violated = true;
        if (o.state == SafetyState::DEGRADED && o.speed_mm_s > 250) r.violated = true;
        if (in.risk_valid && o.speed_mm_s > in.max_safe_speed_mm_s) r.violated = true;
        if (o.speed_mm_s > 1000) r.violated = true;
        if (o.state == SafetyState::DEGRADED) r.saw_degraded = true;
        if (o.state == SafetyState::SAFE_STOP && r.stop_t_ms < 0) r.stop_t_ms = t;
        r.final_state = o.state; r.reason = o.reason; r.final_speed = o.speed_mm_s;
    }
    return r;
}

static void scenario(const char* dir, const char* name, long duration_ms, void (*expect)(const Result&)) {
    std::vector<Fr> fr;
    std::string p = std::string(dir) + "/" + name + ".csv";
    if (!load(p.c_str(), fr)) { printf("FAIL cannot load %s\n", p.c_str()); ++failures; return; }
    Result r = replay(fr, duration_ms);
    CHECK(!r.violated);
    expect(r);
}

int main(int argc, char** argv) {
    test_startup_and_ramp();
    test_recovery_hysteresis_exact();
    test_unhealthy_to_stop_exact();
    test_blip_recovers_without_stop();
    test_age_boundary();
    test_degraded_obeys_max_safe();
    test_unhealthy_breaks_critical_streak();
    test_critical_sustained();
    test_reset_rules();
    test_external_stop_overrides();
    test_monitor();

    if (argc > 1) {
        const char* d = argv[1];
        scenario(d, "approach_stop", 8000, [](const Result& r) {
            CHECK(r.final_speed == 0);  // ends stopped, by policy or by the scenario itself
        });
        scenario(d, "sudden_obstacle", 3000, [](const Result& r) {
            CHECK(r.final_state == SafetyState::SAFE_STOP && r.reason == SafetyReason::CRITICAL_RISK);
            CHECK(r.stop_t_ms >= 1020 && r.stop_t_ms <= 1020 + 5 * 20 + 20);  // within ~5 cycles of the obstacle
        });
        scenario(d, "sensor_stale", 4000, [](const Result& r) {
            CHECK(r.final_state == SafetyState::SAFE_STOP && r.final_speed == 0);
            CHECK(r.stop_t_ms >= 1500 && r.stop_t_ms <= 1500 + 12 * 20);      // 200 ms after data goes stale
        });
        scenario(d, "dropout_recovery", 4000, [](const Result& r) {
            CHECK(r.final_state == SafetyState::SAFE_STOP);                    // 600 ms silence is a stop
            CHECK(r.stop_t_ms >= 1000 && r.stop_t_ms <= 1000 + 100 + 10 * 20 + 20);
        });
        scenario(d, "invalid_frame", 2000, [](const Result& r) {
            CHECK(r.saw_degraded);
            CHECK(r.final_state == SafetyState::NORMAL && r.stop_t_ms < 0);    // 100 ms blip: degrade, recover, never stop
        });
    }
    printf("CONTROL TEST: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
