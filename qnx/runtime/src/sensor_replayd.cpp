// sensor_replayd: plays an expanded scenario CSV into motiond at the scenario's own timestamps.
// Rows absent from the CSV (dropouts) are simply never sent, exactly like a dead sensor.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "edgert_ipc.h"
#include "svc_common.h"

using namespace edgert;

struct Row { long v[14]; };

static bool load(const char* path, std::vector<Row>& rows) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return false; }
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
    const char* csv = nullptr;
    int prio = 0, tail_ms = 1000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tail-ms") && i + 1 < argc) tail_ms = atoi(argv[++i]);
        else { fprintf(stderr, "usage: sensor_replayd --csv FILE [--prio P] [--tail-ms N]\n"); return 2; }
    }
    std::vector<Row> rows;
    if (!csv || !load(csv, rows)) { fprintf(stderr, "sensor_replayd: cannot load CSV\n"); return 2; }
    install_stop_handlers();
    if (set_fifo_priority(prio) != 0) { perror("sensor_replayd: set priority"); return 1; }
    Client* c = connect_retry(NAME_MOTIOND, 10000);
    if (!c) { fprintf(stderr, "sensor_replayd: cannot reach motiond\n"); return 1; }

    const uint64_t start = now_ns() + 100ULL * 1000000ULL;
    uint64_t sent = 0, errors = 0;
    for (const Row& r : rows) {
        if (g_stop) break;
        const uint64_t at = start + static_cast<uint64_t>(r.v[1]) * 1000000ULL;
        timespec ts;
        ts.tv_sec = static_cast<time_t>(at / 1000000000ULL);
        ts.tv_nsec = static_cast<long>(at % 1000000000ULL);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

        SensorMsg m;
        memset(&m, 0, sizeof m);
        m.header = make_header(MsgType::SENSOR_FRAME);
        m.sequence = static_cast<uint32_t>(r.v[0]);
        m.frame.monotonic_timestamp_ns = now_ns();
        m.frame.sequence = m.sequence;
        m.frame.flags = static_cast<uint8_t>(r.v[2]);
        m.frame.obstacle_distance_mm = static_cast<uint32_t>(r.v[3]);
        m.frame.closing_speed_mm_s = static_cast<int32_t>(r.v[4]);
        m.frame.ego_speed_mm_s = static_cast<uint32_t>(r.v[5]);
        m.frame.sensor_age_ms = static_cast<uint16_t>(r.v[6]);
        m.frame.confidence_u8 = static_cast<uint8_t>(r.v[7]);
        Ack a;
        if (client_call(c, &m, sizeof m, &a, sizeof a, 50) != static_cast<int>(sizeof a)) ++errors;
        ++sent;
    }
    sleep_ms(static_cast<unsigned>(tail_ms));  // keep the process (and the pipeline) observable after the last frame
    printf("sensor_replayd: done sent=%llu errors=%llu\n", (unsigned long long)sent, (unsigned long long)errors);
    client_close(c);
    return errors ? 1 : 0;
}
