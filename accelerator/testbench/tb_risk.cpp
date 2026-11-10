// Self-checking testbench for the RACE kernel.
//   tb_risk <vectors.csv> [more.csv ...]   check CSV vectors (host runs, 10000+ rows)
//   tb_risk                                check the embedded vectors (Vitis csim/cosim)
// Consecutive rows with the same configuration are sent through the kernel as one
// batch, so the batch path (count > 1) is exercised as well as count == 1.
// Returns 0 on pass, non-zero on any mismatch (required by Vitis csim/cosim).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "embedded_vectors.h"
#include "risk_kernel.h"

using namespace edgert;

namespace {

constexpr int kCols = 25;  // 5 inputs, 14 config, 6 outputs

struct Row {
    RiskInput in;
    RiskConfig cfg;
    RiskOutput want;
};

Row to_row(const long long v[kCols]) {
    Row r;
    r.in.obstacle_distance_mm = static_cast<uint32_t>(v[0]);
    r.in.closing_speed_mm_s = static_cast<int32_t>(v[1]);
    r.in.ego_speed_mm_s = static_cast<uint32_t>(v[2]);
    r.in.sensor_age_ms = static_cast<uint16_t>(v[3]);
    r.in.confidence_u8 = static_cast<uint8_t>(v[4]);
    r.cfg.decel_mm_s2 = static_cast<uint32_t>(v[5]);
    r.cfg.reaction_time_ms = static_cast<uint16_t>(v[6]);
    r.cfg.margin_mm = static_cast<uint16_t>(v[7]);
    r.cfg.t_warn_ms = static_cast<uint16_t>(v[8]);
    r.cfg.t_crit_ms = static_cast<uint16_t>(v[9]);
    r.cfg.min_confidence_u8 = static_cast<uint8_t>(v[10]);
    r.cfg.max_sensor_age_ms = static_cast<uint16_t>(v[11]);
    r.cfg.level_medium_q15 = static_cast<uint16_t>(v[12]);
    r.cfg.level_high_q15 = static_cast<uint16_t>(v[13]);
    r.cfg.level_critical_q15 = static_cast<uint16_t>(v[14]);
    r.cfg.v_max_mm_s = static_cast<uint32_t>(v[15]);
    r.cfg.v_restricted_mm_s = static_cast<uint32_t>(v[16]);
    r.cfg.v_crawl_mm_s = static_cast<uint32_t>(v[17]);
    r.cfg.v_invalid_mm_s = static_cast<uint32_t>(v[18]);
    r.want.risk_q15 = static_cast<uint16_t>(v[19]);
    r.want.risk_level = static_cast<uint16_t>(v[20]);
    r.want.time_to_collision_ms = static_cast<uint32_t>(v[21]);
    r.want.stopping_distance_mm = static_cast<uint32_t>(v[22]);
    r.want.max_safe_speed_mm_s = static_cast<uint32_t>(v[23]);
    r.want.status = static_cast<uint16_t>(v[24]);
    return r;
}

bool cfg_equal(const RiskConfig& a, const RiskConfig& b) {
    return a.decel_mm_s2 == b.decel_mm_s2 && a.reaction_time_ms == b.reaction_time_ms && a.margin_mm == b.margin_mm &&
           a.t_warn_ms == b.t_warn_ms && a.t_crit_ms == b.t_crit_ms && a.min_confidence_u8 == b.min_confidence_u8 &&
           a.max_sensor_age_ms == b.max_sensor_age_ms && a.level_medium_q15 == b.level_medium_q15 &&
           a.level_high_q15 == b.level_high_q15 && a.level_critical_q15 == b.level_critical_q15 &&
           a.v_max_mm_s == b.v_max_mm_s && a.v_restricted_mm_s == b.v_restricted_mm_s &&
           a.v_crawl_mm_s == b.v_crawl_mm_s && a.v_invalid_mm_s == b.v_invalid_mm_s;
}

bool out_equal(const RiskOutput& a, const RiskOutput& b) {
    return a.risk_q15 == b.risk_q15 && a.risk_level == b.risk_level && a.time_to_collision_ms == b.time_to_collision_ms &&
           a.stopping_distance_mm == b.stopping_distance_mm && a.max_safe_speed_mm_s == b.max_safe_speed_mm_s &&
           a.status == b.status;
}

bool load_csv(const char* path, std::vector<Row>& rows) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    std::string line;
    int c;
    bool header = true;
    while (true) {
        line.clear();
        while ((c = std::fgetc(f)) != EOF && c != '\n') line.push_back(static_cast<char>(c));
        if (line.empty() && c == EOF) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (header) {
            header = false;
            continue;
        }
        if (line.empty()) continue;
        long long v[kCols];
        const char* p = line.c_str();
        for (int i = 0; i < kCols; ++i) {
            char* end = nullptr;
            v[i] = std::strtoll(p, &end, 10);
            if (end == p || (i < kCols - 1 && *end != ',')) {
                std::fprintf(stderr, "%s: bad row: %s\n", path, line.c_str());
                std::fclose(f);
                return false;
            }
            p = end + 1;
        }
        rows.push_back(to_row(v));
    }
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<Row> rows;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            if (!load_csv(argv[i], rows)) return 2;
    } else {
        if (kEmbeddedCols != kCols) {
            std::fprintf(stderr, "embedded vector width %d != %d\n", kEmbeddedCols, kCols);
            return 2;
        }
        for (int i = 0; i < kEmbeddedRows; ++i) rows.push_back(to_row(kEmbeddedVectors[i]));
    }

    static RiskInput in[RISK_MAX_SAMPLES];
    static RiskOutput out[RISK_MAX_SAMPLES];
    long long errors = 0, batches = 0;
    size_t i = 0;
    while (i < rows.size()) {
        int n = 0;
        while (i + n < rows.size() && n < RISK_MAX_SAMPLES && cfg_equal(rows[i + n].cfg, rows[i].cfg)) {
            in[n] = rows[i + n].in;
            ++n;
        }
        risk_kernel(in, out, rows[i].cfg, n);
        ++batches;
        for (int k = 0; k < n; ++k) {
            if (!out_equal(out[k], rows[i + k].want)) {
                if (errors < 10)
                    std::printf("MISMATCH row %zu: got risk=%u level=%u ttc=%u stop=%u speed=%u status=%u, want risk=%u "
                                "level=%u ttc=%u stop=%u speed=%u status=%u\n",
                                i + k, out[k].risk_q15, out[k].risk_level, out[k].time_to_collision_ms,
                                out[k].stopping_distance_mm, out[k].max_safe_speed_mm_s, out[k].status,
                                rows[i + k].want.risk_q15, rows[i + k].want.risk_level,
                                rows[i + k].want.time_to_collision_ms, rows[i + k].want.stopping_distance_mm,
                                rows[i + k].want.max_safe_speed_mm_s, rows[i + k].want.status);
                ++errors;
            }
        }
        i += n;
    }
    std::printf("GATE RISK TB: %s (%zu vectors, %lld batches, %lld errors)\n", errors ? "FAIL" : "PASS", rows.size(),
                batches, errors);
    return errors ? 1 : 0;
}
