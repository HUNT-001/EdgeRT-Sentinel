// ipc_test: replays an expanded scenario (CSV) through riskd over real IPC and checks every
// response bit-for-bit against the golden model's expected output, plus negative tests and
// round-trip latency. Exit 0 only if everything passes.
//
// CSV columns (from `edgert scenario expand --format csv`):
//   seq,t_ms,flags,dist,closing,ego,age,conf,risk_q15,risk_level,ttc_ms,stop_mm,vmax,status
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "edgert_ipc.h"
#include "edgert_risk_adapter.h"

using namespace edgert;

struct Row { long v[14]; };

static bool load(const char* path, std::vector<Row>& rows) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return false; }  // header
    while (fgets(line, sizeof line, f)) {
        Row r;
        char* p = line;
        for (int i = 0; i < 14; ++i) {
            char* e;
            r.v[i] = strtol(p, &e, 10);
            if (e == p) { fclose(f); return false; }
            p = (*e == ',') ? e + 1 : e;
        }
        rows.push_back(r);
    }
    fclose(f);
    return !rows.empty();
}

int main(int argc, char** argv) {
    const char* name = "edgert_riskd";
    const char* csv = nullptr;
    long max_p99_us = 0;  // 0 = report only
    int prio = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-p99-us") && i + 1 < argc) max_p99_us = atol(argv[++i]);
        else { fprintf(stderr, "usage: ipc_test --csv FILE [--name N] [--prio P] [--max-p99-us N]\n"); return 2; }
    }
    std::vector<Row> rows;
    if (!csv || !load(csv, rows)) { fprintf(stderr, "ipc_test: cannot load CSV '%s'\n", csv ? csv : "(none)"); return 2; }
    if (set_fifo_priority(prio) != 0) { perror("ipc_test: set priority"); return 1; }

    Client* c = nullptr;
    for (int i = 0; i < 100 && !c; ++i) {  // wait up to ~5 s for riskd to come up
        c = client_open(name);
        if (!c) { timespec ts{0, 50 * 1000 * 1000}; nanosleep(&ts, nullptr); }
    }
    if (!c) { fprintf(stderr, "ipc_test: cannot connect to '%s'\n", name); return 1; }

    int mismatches = 0, io_errors = 0;
    std::vector<uint64_t> rtt;
    rtt.reserve(rows.size());
    for (const Row& r : rows) {
        RiskRequest req;
        memset(&req, 0, sizeof req);
        req.header = make_header(MsgType::RISK_REQUEST);
        req.sequence = static_cast<uint32_t>(r.v[0]);
        req.sensor.monotonic_timestamp_ns = now_ns();
        req.sensor.sequence = req.sequence;
        req.sensor.flags = static_cast<uint8_t>(r.v[2]);
        req.sensor.obstacle_distance_mm = static_cast<uint32_t>(r.v[3]);
        req.sensor.closing_speed_mm_s = static_cast<int32_t>(r.v[4]);
        req.sensor.ego_speed_mm_s = static_cast<uint32_t>(r.v[5]);
        req.sensor.sensor_age_ms = static_cast<uint16_t>(r.v[6]);
        req.sensor.confidence_u8 = static_cast<uint8_t>(r.v[7]);

        RiskResponse rsp;
        memset(&rsp, 0, sizeof rsp);
        uint64_t t0 = now_ns();
        int n = client_call(c, &req, sizeof req, &rsp, sizeof rsp);
        rtt.push_back(now_ns() - t0);
        if (n != static_cast<int>(sizeof rsp)) { ++io_errors; continue; }
        bool ok = risk_response_usable(rsp, req.sequence) == (r.v[13] == RISK_VALID) &&
                  rsp.sequence == req.sequence && rsp.risk_q15 == r.v[8] && rsp.risk_level == r.v[9] &&
                  rsp.time_to_collision_ms == static_cast<uint32_t>(r.v[10]) &&
                  rsp.stopping_distance_mm == static_cast<uint32_t>(r.v[11]) &&
                  rsp.max_safe_speed_mm_s == static_cast<uint32_t>(r.v[12]) && rsp.status == r.v[13];
        if (!ok && ++mismatches <= 5)
            fprintf(stderr, "MISMATCH seq=%ld got q15=%u lvl=%u status=%u want q15=%ld lvl=%ld status=%ld\n", r.v[0],
                    rsp.risk_q15, rsp.risk_level, rsp.status, r.v[8], r.v[9], r.v[13]);
    }

    // Negative tests: wrong version and wrong size must yield a fail-safe response, not a hang or crash.
    int neg_fail = 0;
    {
        RiskRequest bad;
        memset(&bad, 0, sizeof bad);
        bad.header = make_header(MsgType::RISK_REQUEST);
        bad.header.version = 99;
        bad.sequence = 7;
        RiskResponse rsp;
        int n = client_call(c, &bad, sizeof bad, &rsp, sizeof rsp);
        if (n != static_cast<int>(sizeof rsp) || rsp.status != RISK_INVALID_INPUT || rsp.risk_level != RISK_LEVEL_INVALID)
            { fprintf(stderr, "NEG wrong-version: not fail-safe (n=%d status=%u level=%u)\n", n, rsp.status, rsp.risk_level); ++neg_fail; }
        char tiny[3] = {1, 2, 3};
        n = client_call(c, tiny, sizeof tiny, &rsp, sizeof rsp);
        if (n != static_cast<int>(sizeof rsp) || rsp.status != RISK_INVALID_INPUT)
            { fprintf(stderr, "NEG short-message: not fail-safe (n=%d status=%u)\n", n, rsp.status); ++neg_fail; }
    }
    client_close(c);

    std::sort(rtt.begin(), rtt.end());
    auto pct = [&](double p) { return rtt[static_cast<size_t>(p * (rtt.size() - 1))] / 1000.0; };
    double p99 = pct(0.99);
    printf("frames=%zu mismatches=%d io_errors=%d negative_fail=%d\n", rows.size(), mismatches, io_errors, neg_fail);
    printf("rtt_us p50=%.1f p99=%.1f max=%.1f\n", pct(0.5), p99, rtt.back() / 1000.0);
    bool lat_ok = max_p99_us == 0 || p99 <= static_cast<double>(max_p99_us);
    if (!lat_ok) printf("latency p99 %.1f us exceeds budget %ld us\n", p99, max_p99_us);
    bool pass = !mismatches && !io_errors && !neg_fail && lat_ok;
    printf("IPC TEST: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
