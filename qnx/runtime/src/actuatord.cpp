// actuatord: the only place a speed is "applied". It enforces two things regardless of what motiond says:
//   1. a SAFE_STOP from safetyd latches and forces zero speed;
//   2. no ControlCommand for 100 ms forces zero speed until commands resume (a dead or wedged motiond
//      must never leave the last speed applied).
// Log lines (absolute CLOCK_MONOTONIC ms, comparable across processes):
//   ACT t_ms=<ms> applied=<mm/s> src=<init|motiond|safetyd|timeout> reason=<n>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edgert_protocol.h"
#include "svc_common.h"

using namespace edgert;

int main(int argc, char** argv) {
    int prio = 0;
    uint32_t cmd_timeout_ms = 100;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cmd-timeout-ms") && i + 1 < argc) cmd_timeout_ms = static_cast<uint32_t>(atoi(argv[++i]));
        else { fprintf(stderr, "usage: actuatord [--prio P] [--cmd-timeout-ms N]\n"); return 2; }
    }
    install_stop_handlers();
    if (set_fifo_priority(prio) != 0) { perror("actuatord: set priority"); return 1; }
    Server* s = server_open(NAME_ACTUATORD);
    if (!s) { perror("actuatord: server_open"); return 1; }
    server_set_exit_when_idle(s, false);

    bool latched = false, timed_out = false, have_cmd = false;
    SafetyReason latch_reason = SafetyReason::NONE;
    int32_t applied = 0;
    const char* src = "init";
    uint64_t last_cmd_ns = 0, commands = 0, safety_stops = 0, timeouts = 0, bad = 0, max_gap_ms = 0;
    printf("ACT t_ms=%llu applied=0 src=init reason=0\n", (unsigned long long)ms_of(now_ns()));
    fflush(stdout);

    auto apply = [&](int32_t v, const char* why, uint8_t reason) {
        if (v != applied || strcmp(why, src) != 0) {
            applied = v;
            src = why;
            printf("ACT t_ms=%llu applied=%d src=%s reason=%u\n", (unsigned long long)ms_of(now_ns()), v, why, reason);
            fflush(stdout);
        }
    };

    while (!g_stop) {
        union { ControlCommand c; SafetyCommand sc; uint8_t raw[64]; } m;
        size_t n = 0;
        int h = server_receive(s, &m, sizeof m, &n, 10);
        const uint64_t now = now_ns();
        if (h >= 0) {
            const MsgHeader hd = *reinterpret_cast<const MsgHeader*>(&m);
            if (hd.version == PROTOCOL_VERSION && hd.type == static_cast<uint16_t>(MsgType::CONTROL_COMMAND) &&
                n == sizeof(ControlCommand)) {
                ++commands;
                if (have_cmd) {
                    uint64_t gap = ms_of(now - last_cmd_ns);
                    if (gap > max_gap_ms) max_gap_ms = gap;
                }
                have_cmd = true;
                last_cmd_ns = now;
                timed_out = false;
                const int32_t want = m.c.left_velocity_mm_s;
                if (latched) apply(0, "safetyd", static_cast<uint8_t>(latch_reason));
                else apply(want, "motiond", m.c.reason);
                ActuatorAck a;
                memset(&a, 0, sizeof a);
                a.header = make_header(MsgType::ACTUATOR_ACK);
                a.sequence = m.c.sequence;
                a.applied_left_mm_s = a.applied_right_mm_s = applied;
                a.state = static_cast<uint8_t>(latched ? SafetyState::SAFE_STOP : SafetyState::NORMAL);
                a.reason = static_cast<uint8_t>(latch_reason);
                a.safety_override = latched ? 1 : 0;
                server_reply(s, h, &a, sizeof a);
            } else if (hd.version == PROTOCOL_VERSION && hd.type == static_cast<uint16_t>(MsgType::SAFETY_COMMAND) &&
                       n == sizeof(SafetyCommand)) {
                if (m.sc.state == static_cast<uint8_t>(SafetyState::SAFE_STOP) && !latched) {
                    latched = true;
                    latch_reason = static_cast<SafetyReason>(m.sc.reason);
                    ++safety_stops;
                    apply(0, "safetyd", m.sc.reason);
                }
                Ack a;
                memset(&a, 0, sizeof a);
                a.header = make_header(MsgType::GENERIC_ACK);
                a.sequence = m.sc.sequence;
                server_reply(s, h, &a, sizeof a);
            } else {
                ++bad;
                Ack a;
                memset(&a, 0, sizeof a);
                a.header = make_header(MsgType::GENERIC_ACK);
                server_reply(s, h, &a, sizeof a);  // never leave a sender blocked
            }
        }
        if (have_cmd && !latched && !timed_out && ms_of(now - last_cmd_ns) > cmd_timeout_ms) {
            timed_out = true;
            ++timeouts;
            apply(0, "timeout", static_cast<uint8_t>(SafetyReason::ACTUATOR_TIMEOUT));
        }
    }
    printf("actuatord: stopped commands=%llu safety_stops=%llu timeouts=%llu bad=%llu max_cmd_gap_ms=%llu\n",
           (unsigned long long)commands, (unsigned long long)safety_stops, (unsigned long long)timeouts,
           (unsigned long long)bad, (unsigned long long)max_gap_ms);
    server_close(s);
    return 0;
}
