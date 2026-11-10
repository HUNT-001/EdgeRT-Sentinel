// riskd: stateless RACE service. One request in, one response out, fixed work per call.
// Malformed or wrong-version requests get a fail-safe response, never silence, so the
// caller's sequence check and timeout logic stay meaningful.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edgert_ipc.h"
#include "edgert_risk_adapter.h"

using namespace edgert;

int main(int argc, char** argv) {
    const char* name = "edgert_riskd";
    int prio = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
        else { fprintf(stderr, "usage: riskd [--name N] [--prio P]\n"); return 2; }
    }
    if (set_fifo_priority(prio) != 0) { perror("riskd: set priority"); return 1; }
    Server* s = server_open(name);
    if (!s) { perror("riskd: server_open"); return 1; }
    const RiskConfig cfg = default_risk_config();
    fprintf(stderr, "riskd: serving '%s' prio=%d protocol=v%u\n", name, prio, PROTOCOL_VERSION);

    uint64_t served = 0, rejected = 0;
    for (;;) {
        RiskRequest req;
        size_t n = 0;
        int h = server_receive(s, &req, sizeof req, &n);
        if (h == -2) break;
        if (h < 0) { perror("riskd: receive"); server_close(s); return 1; }

        RiskOutput out;
        uint32_t seq = 0;
        if (n == sizeof req && req.header.type == static_cast<uint16_t>(MsgType::RISK_REQUEST) &&
            req.header.version == PROTOCOL_VERSION) {
            seq = req.sequence;
            out = risk_eval(to_risk_input(req.sensor), cfg);
        } else {
            ++rejected;
            RiskInput bad{};
            bad.obstacle_distance_mm = RISK_DIST_MAX_MM + 1;  // forces the invalid-input fail-safe
            out = risk_eval(bad, cfg);
        }
        RiskResponse rsp = make_risk_response(seq, now_ns(), out);
        if (server_reply(s, h, &rsp, sizeof rsp) != 0) perror("riskd: reply");
        ++served;
    }
    fprintf(stderr, "riskd: stopped served=%llu rejected=%llu\n", (unsigned long long)served,
            (unsigned long long)rejected);
    server_close(s);
    return 0;
}
