// Host-side check of the IPC contract. The layout is also enforced at compile time
// by static_asserts in edgert_protocol.h; building this file is the main test.
#include <stdio.h>
#include <string.h>

#include "edgert_risk_adapter.h"

using namespace edgert;

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                          \
        }                                                        \
    } while (0)

int main() {
    // Risk enums in the shared header keep the values the spec documents.
    static_assert(RISK_VALID == 0 && RISK_STALE_SENSOR == 1 && RISK_LOW_CONFIDENCE == 2 && RISK_INVALID_INPUT == 3 &&
                      RISK_COMPUTATION_ERROR == 4,
                  "RiskStatus values changed");
    static_assert(static_cast<int>(SafetyState::SAFE_STOP) == 2, "SafetyState values changed");

    // A frame the sensor flags invalid is rejected by the core, never evaluated.
    SensorFrame f;
    memset(&f, 0, sizeof f);
    f.obstacle_distance_mm = 5000;
    f.closing_speed_mm_s = 800;
    f.ego_speed_mm_s = 800;
    f.confidence_u8 = 240;
    RiskConfig cfg = default_risk_config();
    CHECK(risk_eval(to_risk_input(f), cfg).status == RISK_VALID);
    f.flags = SENSOR_FLAG_INVALID;
    CHECK(risk_eval(to_risk_input(f), cfg).status == RISK_INVALID_INPUT);

    // Response round trip through raw bytes, as MsgReply would deliver it.
    f.flags = 0;
    RiskOutput o = risk_eval(to_risk_input(f), cfg);
    RiskResponse sent = make_risk_response(42, 123456789ULL, o);
    unsigned char wire[sizeof(RiskResponse)];
    memcpy(wire, &sent, sizeof wire);
    RiskResponse got;
    memcpy(&got, wire, sizeof got);
    CHECK(risk_response_usable(got, 42));
    CHECK(!risk_response_usable(got, 43));  // wrong sequence
    got.header.version = 2;
    CHECK(!risk_response_usable(got, 42));  // wrong protocol version
    got.header.version = PROTOCOL_VERSION;
    got.status = RISK_STALE_SENSOR;
    CHECK(!risk_response_usable(got, 42));  // core said not valid

    // Reserved bytes are zeroed so padding never leaks stale stack data.
    CHECK(sent.reserved0 == 0 && sent.reserved1 == 0);

    printf("PROTOCOL TEST: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
