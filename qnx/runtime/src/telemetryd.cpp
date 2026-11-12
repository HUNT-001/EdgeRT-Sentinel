// telemetryd: collects TelemetryEvent from every service and prints one JSON object per line.
//   {"t_ms":..,"src":"motion","ev":"control_cycle","seq":..,"v0":..,"v1":..,"v2":..}
// Per-source sequence gaps (events dropped by a sender) are reported as {"ev":"gap",...}.
// The host workbench reads this stream; on QNX it goes to the console / a log file.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svc_common.h"
#include "tele_tx.h"

using namespace edgert;

static const char* src_name(uint16_t s) {
    static const char* n[] = {"?", "sensor", "risk", "motion", "safety", "actuator", "telemetry"};
    return s < 7 ? n[s] : "?";
}
static const char* ev_name(uint16_t e) {
    switch (static_cast<EventCode>(e)) {
        case EventCode::SERVICE_START: return "service_start";
        case EventCode::CONTROL_CYCLE: return "control_cycle";
        case EventCode::DEADLINE_MISS: return "deadline_miss";
        case EventCode::SENSOR_STALE: return "sensor_stale";
        case EventCode::RISK_REJECTED: return "risk_rejected";
        case EventCode::STATE_CHANGE: return "state_change";
        case EventCode::WATCHDOG_TRIP: return "watchdog_trip";
        case EventCode::OPERATOR_RESET: return "operator_reset";
        case EventCode::ACTUATOR_APPLIED: return "actuator_applied";
        default: return "other";
    }
}

int main(int argc, char** argv) {
    int prio = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
        else { fprintf(stderr, "usage: telemetryd [--prio P]\n"); return 2; }
    }
    install_stop_handlers();
    if (set_fifo_priority(prio) != 0) { perror("telemetryd: set priority"); return 1; }
    Server* s = server_open(NAME_TELEMETRYD);
    if (!s) { perror("telemetryd: server_open"); return 1; }
    server_set_exit_when_idle(s, false);
    uint32_t last_seq[8] = {0};
    uint64_t events = 0, gaps = 0, bad = 0;
    while (!g_stop) {
        TelemetryEvent e;
        size_t n = 0;
        int h = server_receive(s, &e, sizeof e, &n, 100);
        if (h == RX_TIMEOUT || h == RX_PULSE) continue;
        if (h < 0) break;
        Ack a;
        memset(&a, 0, sizeof a);
        a.header = make_header(MsgType::GENERIC_ACK);
        server_reply(s, h, &a, sizeof a);
        if (n != sizeof e || e.header.type != static_cast<uint16_t>(MsgType::TELEMETRY_EVENT) ||
            e.header.version != PROTOCOL_VERSION || e.source == 0 || e.source > 6) { ++bad; continue; }
        ++events;
        uint32_t& last = last_seq[e.source];
        if (last != 0 && e.sequence > last + 1) {
            gaps += e.sequence - last - 1;
            printf("{\"t_ms\":%llu,\"src\":\"%s\",\"ev\":\"gap\",\"lost\":%u}\n", (unsigned long long)ms_of(e.monotonic_timestamp_ns),
                   src_name(e.source), e.sequence - last - 1);
        }
        last = e.sequence;
        printf("{\"t_ms\":%llu,\"src\":\"%s\",\"ev\":\"%s\",\"seq\":%u,\"v0\":%u,\"v1\":%d,\"v2\":%u}\n",
               (unsigned long long)ms_of(e.monotonic_timestamp_ns), src_name(e.source), ev_name(e.event), e.sequence, e.value0,
               e.value1, e.value2);
        fflush(stdout);
    }
    fprintf(stderr, "telemetryd: stopped events=%llu lost=%llu bad=%llu\n", (unsigned long long)events,
            (unsigned long long)gaps, (unsigned long long)bad);
    server_close(s);
    return 0;
}
