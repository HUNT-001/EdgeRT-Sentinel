// motiond: the 50 Hz control loop.
//   receiver thread : takes SensorMsg from sensor_replayd, keeps only the latest frame, acks at once.
//   control thread  : timer tick -> snapshot frame -> riskd (timed call) -> Controller::step ->
//                     ControlCommand to actuatord (timed call) -> heartbeat pulse to safetyd.
// The heartbeat is sent LAST in the cycle, so it proves the whole cycle completed.
// Fault injection for tests:  --crash-after N  (SIGKILL self after N control cycles)
//                             --wedge-after N  (stop all work but keep ticking and pulsing a frozen counter)
// Log lines (absolute CLOCK_MONOTONIC ms):
//   MOTION t_ms=<ms> cycle=<n> state=<0 normal|1 degraded|2 safe_stop> reason=<n> speed=<mm/s>   (on change)
//   MOTION FAULT <crash|wedge> t_ms=<ms> cycle=<n>
//   MOTION RESET <done|rejected> t_ms=<ms> cycle=<n>        (operator reset outcome; see opctl)
//   MOTION summary cycles=.. misses=.. late=.. max_late_us=.. max_exec_us=.. riskd_err=.. act_err=..
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edgert_control.h"
#include "edgert_risk_adapter.h"
#include "svc_common.h"
#include "tele_tx.h"

using namespace edgert;

struct Shared {
    pthread_mutex_t mu;
    SensorFrame frame;
    uint64_t rx_ns;
    bool have;
    uint64_t frames_rx, bad_msgs;
    uint8_t state, reason;     // published by the control thread for operator replies
    uint32_t reset_hold;       // cycles for which the controller is asked to leave SAFE_STOP
};

static Shared g_sh;
static Server* g_srv;
static int g_recv_prio = 0;
static uint32_t g_reset_hold_cycles = 25;  // 500 ms to prove healthy inputs after an operator reset

static void* receiver(void*) {
    set_fifo_priority(g_recv_prio);
    Client* safe_rx = connect_retry(NAME_SAFETYD, 10000);  // own connection: never shared with the control thread
    while (!g_stop) {
        union { SensorMsg m; OperatorCommand oc; uint8_t raw[64]; } u;
        SensorMsg& m = u.m;
        size_t n = 0;
        int h = server_receive(g_srv, &u, sizeof u, &n, 100);
        if (h == RX_TIMEOUT || h == RX_PULSE) continue;
        if (h < 0) break;
        if (n == sizeof(OperatorCommand) && u.oc.header.type == static_cast<uint16_t>(MsgType::OPERATOR_COMMAND) &&
            u.oc.header.version == PROTOCOL_VERSION) {
            OperatorAck ak;
            memset(&ak, 0, sizeof ak);
            ak.header = make_header(MsgType::OPERATOR_ACK);
            ak.sequence = u.oc.sequence;
            pthread_mutex_lock(&g_sh.mu);
            const uint8_t st = g_sh.state;
            pthread_mutex_unlock(&g_sh.mu);
            if (u.oc.command != static_cast<uint8_t>(OperatorCmd::RESET)) {
                ak.result = static_cast<uint8_t>(OperatorResult::UNSUPPORTED);
            } else if (st != static_cast<uint8_t>(SafetyState::SAFE_STOP)) {
                ak.result = static_cast<uint8_t>(OperatorResult::NOT_LATCHED);
            } else {  // safetyd decides first; only then is the controller asked to leave SAFE_STOP
                OperatorCommand fwd = u.oc;
                OperatorAck sa;
                memset(&sa, 0, sizeof sa);
                const int sn = safe_rx ? client_call(safe_rx, &fwd, sizeof fwd, &sa, sizeof sa, 50) : -1;
                if (sn == static_cast<int>(sizeof sa) && sa.result == static_cast<uint8_t>(OperatorResult::OK)) {
                    pthread_mutex_lock(&g_sh.mu);
                    g_sh.reset_hold = g_reset_hold_cycles;
                    pthread_mutex_unlock(&g_sh.mu);
                    ak.result = static_cast<uint8_t>(OperatorResult::OK);
                } else {
                    ak.result = static_cast<uint8_t>(OperatorResult::REJECTED);
                    printf("MOTION RESET rejected t_ms=%llu by=safetyd\n", (unsigned long long)ms_of(now_ns()));
                    fflush(stdout);
                }
            }
            ak.state = st;
            server_reply(g_srv, h, &ak, sizeof ak);
            continue;
        }
        Ack a;
        memset(&a, 0, sizeof a);
        a.header = make_header(MsgType::GENERIC_ACK);
        if (n == sizeof m && m.header.type == static_cast<uint16_t>(MsgType::SENSOR_FRAME) &&
            m.header.version == PROTOCOL_VERSION) {
            pthread_mutex_lock(&g_sh.mu);
            g_sh.frame = m.frame;
            g_sh.rx_ns = now_ns();
            g_sh.have = true;
            ++g_sh.frames_rx;
            pthread_mutex_unlock(&g_sh.mu);
            a.sequence = m.sequence;
        } else {
            pthread_mutex_lock(&g_sh.mu);
            ++g_sh.bad_msgs;
            pthread_mutex_unlock(&g_sh.mu);
        }
        server_reply(g_srv, h, &a, sizeof a);
    }
    if (safe_rx) client_close(safe_rx);
    return nullptr;
}

int main(int argc, char** argv) {
    int prio = 0;
    long crash_after = -1, wedge_after = -1;
    ControllerConfig cc;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--recv-prio") && i + 1 < argc) g_recv_prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--crash-after") && i + 1 < argc) crash_after = atol(argv[++i]);
        else if (!strcmp(argv[i], "--wedge-after") && i + 1 < argc) wedge_after = atol(argv[++i]);
        else if (!strcmp(argv[i], "--reset-hold") && i + 1 < argc) g_reset_hold_cycles = static_cast<uint32_t>(atoi(argv[++i]));
        else { fprintf(stderr, "usage: motiond [--prio P] [--recv-prio P] [--crash-after N] [--wedge-after N] [--reset-hold CYCLES]\n"); return 2; }
    }
    if (g_recv_prio == 0 && prio > 5) g_recv_prio = prio - 5;  // receiver must never preempt the control loop
    install_stop_handlers();
    TeleTx tele(ServiceId::MOTION, prio > 0 ? 20 : 0);
    tele.emit(EventCode::SERVICE_START);

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);  // control thread never inverted behind the receiver
    pthread_mutex_init(&g_sh.mu, &ma);

    g_srv = server_open(NAME_MOTIOND);
    if (!g_srv) { perror("motiond: server_open"); return 1; }
    server_set_exit_when_idle(g_srv, false);
    pthread_t rt;
    if (pthread_create(&rt, nullptr, receiver, nullptr) != 0) { perror("motiond: pthread_create"); return 1; }

    Client* risk = connect_retry(NAME_RISKD, 10000);
    Client* act = connect_retry(NAME_ACTUATORD, 10000);
    Client* safe = connect_retry(NAME_SAFETYD, 10000);
    if (!risk || !act || !safe) { fprintf(stderr, "motiond: cannot reach riskd/actuatord/safetyd\n"); g_stop = 1; }

    if (!g_stop && set_fifo_priority(prio) != 0) { perror("motiond: set priority"); g_stop = 1; }
    Ticker* tk = g_stop ? nullptr : ticker_open(cc.cycle_ms);
    if (!g_stop && !tk) { perror("motiond: ticker_open"); g_stop = 1; }

    Controller ctl(cc);
    uint32_t tick = 0, ctl_cycles = 0, risk_seq = 0;
    bool override_stop = false;
    SafetyReason override_reason = SafetyReason::NONE;
    bool first_log = true, wedged = false;
    SafetyState last_state = SafetyState::DEGRADED;
    SafetyReason last_reason = SafetyReason::NONE;
    uint64_t misses = 0, late = 0, max_late_ns = 0, max_exec_ns = 0, riskd_err = 0, act_err = 0;
    fprintf(stderr, "motiond: running %u ms cycle, prio=%d recv_prio=%d\n", cc.cycle_ms, prio, g_recv_prio);

    while (!g_stop) {
        uint64_t sched = 0;
        if (!ticker_wait(tk, &sched)) continue;
        const uint64_t wake = now_ns();
        ++tick;
        const uint64_t lateness = wake > sched ? wake - sched : 0;
        if (lateness > max_late_ns) max_late_ns = lateness;
        if (lateness > 5ULL * 1000000ULL) ++late;

        pthread_mutex_lock(&g_sh.mu);
        const bool have = g_sh.have;
        const uint32_t hold_req = g_sh.reset_hold;
        const SensorFrame frame = g_sh.frame;
        const uint64_t rx_ns = g_sh.rx_ns;
        pthread_mutex_unlock(&g_sh.mu);

        if (!have) {  // waiting for the first sensor frame: stay alive, do no control
            client_pulse(safe, PULSE_HEARTBEAT, tick);
            continue;
        }
        if (wedge_after >= 0 && static_cast<long>(ctl_cycles) >= wedge_after) {
            if (!wedged) {
                wedged = true;
                printf("MOTION FAULT wedge t_ms=%llu cycle=%u\n", (unsigned long long)ms_of(now_ns()), ctl_cycles);
                fflush(stdout);
            }
            client_pulse(safe, PULSE_HEARTBEAT, ctl_cycles);  // frozen counter value
            continue;
        }
        if (crash_after >= 0 && static_cast<long>(ctl_cycles) >= crash_after) {
            printf("MOTION FAULT crash t_ms=%llu cycle=%u\n", (unsigned long long)ms_of(now_ns()), ctl_cycles);
            fflush(stdout);
            raise(SIGKILL);
        }

        // 1. risk evaluation of the freshest frame (timed call; a hung riskd cannot stall the loop)
        RiskRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.header = make_header(MsgType::RISK_REQUEST);
        rq.sequence = ++risk_seq;
        rq.sensor = frame;
        RiskResponse rp;
        memset(&rp, 0, sizeof rp);
        const int rn = client_call(risk, &rq, sizeof rq, &rp, sizeof rp, 10);
        const bool rok = rn == static_cast<int>(sizeof rp) && risk_response_usable(rp, rq.sequence);
        if (!rok) { ++riskd_err; tele.emit(EventCode::RISK_REJECTED, rq.sequence); }

        // 2. policy
        CycleInput in;
        memset(&in, 0, sizeof in);
        in.risk_valid = rok;
        in.risk_level = rok ? rp.risk_level : static_cast<uint16_t>(RISK_LEVEL_INVALID);
        in.max_safe_speed_mm_s = rok ? rp.max_safe_speed_mm_s : 0;
        in.input_age_ms = static_cast<uint32_t>(frame.sensor_age_ms + (wake > rx_ns ? ms_of(wake - rx_ns) : 0));
        in.sensor_stale = rn == static_cast<int>(sizeof rp) && rp.status == RISK_STALE_SENSOR;
        in.external_stop = override_stop;
        in.reset_request = hold_req > 0 && ctl.state() == SafetyState::SAFE_STOP;
        if (in.reset_request) in.external_stop = false;  // safetyd already released the actuator latch
        in.external_reason = override_reason;
        const CycleOutput out = ctl.step(in);
        pthread_mutex_lock(&g_sh.mu);
        g_sh.state = static_cast<uint8_t>(out.state);
        g_sh.reason = static_cast<uint8_t>(out.reason);
        if (g_sh.reset_hold > 0) {
            const bool done = out.state != SafetyState::SAFE_STOP;
            if (done || --g_sh.reset_hold == 0) {
                g_sh.reset_hold = 0;
                printf("MOTION RESET %s t_ms=%llu cycle=%u\n", done ? "done" : "rejected", (unsigned long long)ms_of(wake), tick);
                fflush(stdout);
                tele.emit(EventCode::OPERATOR_RESET, done ? 10u : 11u);
            }
        }
        pthread_mutex_unlock(&g_sh.mu);

        // 3. command
        ControlCommand cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.header = make_header(MsgType::CONTROL_COMMAND);
        cmd.sequence = tick;
        cmd.monotonic_timestamp_ns = wake;
        cmd.left_velocity_mm_s = cmd.right_velocity_mm_s = static_cast<int32_t>(out.speed_mm_s);
        cmd.state = static_cast<uint8_t>(out.state);
        cmd.reason = static_cast<uint8_t>(out.reason);
        ActuatorAck ak;
        memset(&ak, 0, sizeof ak);
        const int an = client_call(act, &cmd, sizeof cmd, &ak, sizeof ak, 10);
        if (an == static_cast<int>(sizeof ak) && ak.header.type == static_cast<uint16_t>(MsgType::ACTUATOR_ACK)) {
            override_stop = ak.safety_override != 0;
            override_reason = static_cast<SafetyReason>(ak.reason);
        } else {
            ++act_err;
        }

        if (first_log || out.state != last_state || out.reason != last_reason) {
            printf("MOTION t_ms=%llu cycle=%u state=%u reason=%u speed=%u\n", (unsigned long long)ms_of(wake), tick,
                   (unsigned)out.state, (unsigned)out.reason, out.speed_mm_s);
            fflush(stdout);
            first_log = false;
            last_state = out.state;
            last_reason = out.reason;
            tele.emit(EventCode::STATE_CHANGE, static_cast<uint32_t>(out.state), static_cast<int32_t>(out.reason), tick);
        }
        if (tick % 5 == 0)  // 10 Hz live view: speed, distance, packed risk/state
            tele.emit(EventCode::CONTROL_CYCLE, out.speed_mm_s, static_cast<int32_t>(frame.obstacle_distance_mm),
                      (rok ? rp.risk_q15 : 0u) | ((rok ? rp.risk_level : 4u) << 16) | (static_cast<uint32_t>(out.state) << 20) |
                          (static_cast<uint32_t>(out.reason) << 24));
        ++ctl_cycles;

        // 4. heartbeat last: proves the cycle completed
        client_pulse(safe, PULSE_HEARTBEAT, tick);

        const uint64_t exec = now_ns() - wake;
        if (exec > max_exec_ns) max_exec_ns = exec;
        if (exec > static_cast<uint64_t>(cc.cycle_ms) * 1000000ULL) { ++misses; tele.emit(EventCode::DEADLINE_MISS, static_cast<uint32_t>(exec / 1000)); }  // finished after the next release
    }

    printf("MOTION summary cycles=%u misses=%llu late=%llu max_late_us=%llu max_exec_us=%llu riskd_err=%llu act_err=%llu "
           "frames_rx=%llu\n",
           ctl_cycles, (unsigned long long)misses, (unsigned long long)late, (unsigned long long)(max_late_ns / 1000),
           (unsigned long long)(max_exec_ns / 1000), (unsigned long long)riskd_err, (unsigned long long)act_err,
           (unsigned long long)g_sh.frames_rx);
    fflush(stdout);
    g_stop = 1;
    pthread_join(rt, nullptr);
    ticker_close(tk);
    server_close(g_srv);
    return 0;
}
