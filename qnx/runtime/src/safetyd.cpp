// safetyd: independent watchdog. Receives only motiond's heartbeat pulses; on loss or freeze it
// commands SAFE_STOP straight to actuatord (never via motiond) and keeps retrying until acknowledged.
//   SAFETYD trip t_ms=<ms> gap_ms=<ms since last heartbeat> reason=<n>
//   SAFETYD reset t_ms=<ms> result=<OperatorResult>
// Operator RESET (from motiond): accepted only while heartbeats are fresh. safetyd clears its own latch and
// itself tells actuatord to release; motiond alone can never unlatch the actuator.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edgert_control.h"
#include "svc_common.h"
#include "tele_tx.h"

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
    TeleTx tele(ServiceId::SAFETY, prio > 0 ? 20 : 0);
    tele.emit(EventCode::SERVICE_START);
    if (set_fifo_priority(prio) != 0) { perror("safetyd: set priority"); return 1; }
    Server* s = server_open(NAME_SAFETYD);
    if (!s) { perror("safetyd: server_open"); return 1; }
    server_set_exit_when_idle(s, false);
    Client* act = connect_retry(NAME_ACTUATORD, 10000);
    if (!act) { fprintf(stderr, "safetyd: cannot reach actuatord\n"); return 1; }

    SafetyMonitor mon(now_ns(), mc);
    uint64_t last_hb_ns = now_ns(), beats = 0;
    bool announced = false, acked = false;
    uint32_t seq = 0, last_cycle_seen = 0;
    fprintf(stderr, "safetyd: watching heartbeat timeout=%u ms grace=%u ms\n", mc.heartbeat_timeout_ms, mc.startup_grace_ms);

    while (!g_stop) {
        alignas(8) uint8_t buf[64];
        size_t n = 0;
        int h = server_receive(s, buf, sizeof buf, &n, 10);
        uint64_t now = now_ns();
        if (h == RX_PULSE) {
            int8_t code;
            uint32_t val;
            server_last_pulse(s, &code, &val);
            if (code == PULSE_HEARTBEAT) { mon.heartbeat(now, val); last_hb_ns = now; last_cycle_seen = val; ++beats; }
        } else if (h >= 0) {
            OperatorCommand oc;
            memcpy(&oc, buf, sizeof oc);
            if (n == sizeof oc && oc.header.version == PROTOCOL_VERSION &&
                oc.header.type == static_cast<uint16_t>(MsgType::OPERATOR_COMMAND) &&
                oc.command == static_cast<uint8_t>(OperatorCmd::RESET)) {
                OperatorResult res = OperatorResult::REJECTED;
                const bool fresh = ms_of(now - last_hb_ns) <= mc.heartbeat_timeout_ms && beats > 0;
                if (fresh) {
                    SafetyCommand rel;
                    memset(&rel, 0, sizeof rel);
                    rel.header = make_header(MsgType::SAFETY_COMMAND);
                    rel.sequence = ++seq;
                    rel.monotonic_timestamp_ns = now;
                    rel.state = static_cast<uint8_t>(SafetyState::DEGRADED);  // release the actuator latch
                    Ack ra;
                    if (client_call(act, &rel, sizeof rel, &ra, sizeof ra, 10) == static_cast<int>(sizeof ra)) {
                        mon.reset(now);
                        mon.heartbeat(now, last_cycle_seen);  // keep the monitor armed from the live heartbeat
                        announced = acked = false;
                        res = OperatorResult::OK;
                    }
                }
                printf("SAFETYD reset t_ms=%llu result=%u\n", (unsigned long long)ms_of(now), (unsigned)res);
                fflush(stdout);
                tele.emit(EventCode::OPERATOR_RESET, static_cast<uint32_t>(res));
                OperatorAck ak;
                memset(&ak, 0, sizeof ak);
                ak.header = make_header(MsgType::OPERATOR_ACK);
                ak.sequence = oc.sequence;
                ak.result = static_cast<uint8_t>(res);
                server_reply(s, h, &ak, sizeof ak);
            } else {
                Ack a;  // unexpected message: answer so the sender is never left blocked
                memset(&a, 0, sizeof a);
                a.header = make_header(MsgType::GENERIC_ACK);
                server_reply(s, h, &a, sizeof a);
            }
        }
        if (mon.evaluate(now) && !acked) {
            if (!announced) {
                announced = true;
                tele.emit(EventCode::WATCHDOG_TRIP, static_cast<uint32_t>(ms_of(now - last_hb_ns)));
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
