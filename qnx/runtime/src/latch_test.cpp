// latch_test: plays a fake motiond against a running actuatord + safetyd to check the latch/release path.
//   heartbeat -> stop heartbeats (trip, actuator latched) -> reset refused without heartbeats ->
//   heartbeats resume -> reset accepted -> actuator applies commands again and safetyd does not re-trip.
#include <stdio.h>
#include <string.h>

#include "svc_common.h"

using namespace edgert;

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static Client *g_act, *g_safe;
static uint32_t g_cycle = 0, g_seq = 0;

static void beat_for(int ms, int32_t cmd, ActuatorAck* last) {
    for (int t = 0; t < ms; t += 20) {
        client_pulse(g_safe, PULSE_HEARTBEAT, ++g_cycle);
        if (cmd >= 0) {
            ControlCommand c;
            memset(&c, 0, sizeof c);
            c.header = make_header(MsgType::CONTROL_COMMAND);
            c.sequence = ++g_seq;
            c.left_velocity_mm_s = c.right_velocity_mm_s = cmd;
            ActuatorAck a;
            memset(&a, 0, sizeof a);
            if (client_call(g_act, &c, sizeof c, &a, sizeof a, 20) == static_cast<int>(sizeof a) && last) *last = a;
        }
        sleep_ms(20);
    }
}

static OperatorResult reset() {
    OperatorCommand oc;
    memset(&oc, 0, sizeof oc);
    oc.header = make_header(MsgType::OPERATOR_COMMAND);
    oc.sequence = 1;
    oc.command = static_cast<uint8_t>(OperatorCmd::RESET);
    OperatorAck a;
    memset(&a, 0, sizeof a);
    if (client_call(g_safe, &oc, sizeof oc, &a, sizeof a, 100) != static_cast<int>(sizeof a)) return OperatorResult::UNSUPPORTED;
    return static_cast<OperatorResult>(a.result);
}

int main() {
    g_act = connect_retry(NAME_ACTUATORD, 5000);
    g_safe = connect_retry(NAME_SAFETYD, 5000);
    if (!g_act || !g_safe) { printf("FAIL: services not reachable\n"); return 1; }
    ActuatorAck a;
    memset(&a, 0, sizeof a);

    beat_for(200, 500, &a);
    CHECK(a.applied_left_mm_s == 500 && !a.safety_override, "healthy: command applied");

    sleep_ms(200);  // no heartbeats: safetyd trips and latches the actuator
    beat_for(0, 0, nullptr);
    ControlCommand c;
    memset(&c, 0, sizeof c);
    c.header = make_header(MsgType::CONTROL_COMMAND);
    c.left_velocity_mm_s = c.right_velocity_mm_s = 500;
    CHECK(client_call(g_act, &c, sizeof c, &a, sizeof a, 50) == static_cast<int>(sizeof a), "ack while latched");
    CHECK(a.safety_override == 1 && a.applied_left_mm_s == 0, "latched: speed forced to 0");

    CHECK(reset() == OperatorResult::REJECTED, "reset refused without a fresh heartbeat");
    CHECK(client_call(g_act, &c, sizeof c, &a, sizeof a, 50) == static_cast<int>(sizeof a) && a.safety_override == 1,
          "still latched after refused reset");

    beat_for(100, 0, &a);  // heartbeats are back
    CHECK(reset() == OperatorResult::OK, "reset accepted with fresh heartbeat");
    beat_for(40, 300, &a);
    CHECK(!a.safety_override && a.applied_left_mm_s == 300, "released: command applied again");
    beat_for(400, 300, &a);  // longer than the heartbeat timeout: must not re-trip
    CHECK(!a.safety_override && a.applied_left_mm_s == 300, "no re-trip after reset");

    printf(fails ? "LATCH TEST: FAIL\n" : "LATCH TEST: PASS\n");
    return fails ? 1 : 0;
}
