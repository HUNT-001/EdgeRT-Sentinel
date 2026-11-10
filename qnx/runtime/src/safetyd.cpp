// safetyd: independent watchdog. Receives only motiond's heartbeat pulses; on loss or freeze it
// commands SAFE_STOP straight to actuatord (never via motiond) and keeps retrying until acknowledged.
//   SAFETYD trip t_ms=<ms> gap_ms=<ms since last heartbeat> reason=<n>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edgert_control.h"
#include "svc_common.h"

using namespace edgert;

int main(int argc, char** argv) {
    int prio = 0;
    MonitorConfig mc;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--grace-ms") && i + 1 < argc) mc.startup_grace_ms = static_cast<uint32_t>(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) mc.heartbeat_timeout_ms = static_cast<uint32_t>(atoi(argv[++i]));
        else { fprintf(stderr, "usage: safetyd [--prio P] [--grace-ms N] [--timeout-ms N]\n"); return 2; }
    }
    install_stop_handlers();
    if (set_fifo_priority(prio) != 0) { perror("safetyd: set priority"); return 1; }
    Server* s = server_open(NAME_SAFETYD);
    if (!s) { perror("safetyd: server_open"); return 1; }
    server_set_exit_when_idle(s, false);
    Client* act = connect_retry(NAME_ACTUATORD, 10000);
    if (!act) { fprintf(stderr, "safetyd: cannot reach actuatord\n"); return 1; }

    SafetyMonitor mon(now_ns(), mc);
    uint64_t last_hb_ns = now_ns(), beats = 0;
    bool announced = false, acked = false;
    uint32_t seq = 0;
    fprintf(stderr, "safetyd: watching heartbeat timeout=%u ms grace=%u ms\n", mc.heartbeat_timeout_ms, mc.startup_grace_ms);

    while (!g_stop) {
        uint8_t buf[64];
        size_t n = 0;
        int h = server_receive(s, buf, sizeof buf, &n, 10);
        uint64_t now = now_ns();
        if (h == RX_PULSE) {
            int8_t code;
            uint32_t val;
            server_last_pulse(s, &code, &val);
            if (code == PULSE_HEARTBEAT) { mon.heartbeat(now, val); last_hb_ns = now; ++beats; }
        } else if (h >= 0) {
            Ack a;  // unexpected message: answer so the sender is never left blocked
            memset(&a, 0, sizeof a);
            a.header = make_header(MsgType::GENERIC_ACK);
            server_reply(s, h, &a, sizeof a);
        }
        if (mon.evaluate(now) && !acked) {
            if (!announced) {
                announced = true;
                printf("SAFETYD trip t_ms=%llu gap_ms=%llu reason=%u\n", (unsigned long long)ms_of(now),
                       (unsigned long long)ms_of(now - last_hb_ns), (unsigned)SafetyReason::HEARTBEAT_LOST);
                fflush(stdout);
            }
            SafetyCommand sc;
            memset(&sc, 0, sizeof sc);
            sc.header = make_header(MsgType::SAFETY_COMMAND);
            sc.sequence = ++seq;
            sc.monotonic_timestamp_ns = now;
            sc.state = static_cast<uint8_t>(SafetyState::SAFE_STOP);
            sc.reason = static_cast<uint8_t>(SafetyReason::HEARTBEAT_LOST);
            Ack a;
            if (client_call(act, &sc, sizeof sc, &a, sizeof a, 10) == static_cast<int>(sizeof a)) acked = true;
        }
    }
    printf("safetyd: stopped beats=%llu tripped=%d acked=%d\n", (unsigned long long)beats, mon.tripped() ? 1 : 0, acked ? 1 : 0);
    server_close(s);
    client_close(act);
    return 0;
}
