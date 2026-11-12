#!/usr/bin/env python3
"""Full-range random vectors (random configs, 32-bit extremes) from the golden model.
Usage: gen_full_range.py OUT.csv [N] [SEED]. Same column order as accelerator/vectors."""
import dataclasses, random, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "accelerator" / "reference"))
import risk_model as rm  # noqa: E402

out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 60000
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 11)
R = random.randint


def cfg():
    return rm.RiskConfig(decel_mm_s2=R(0, 40000), reaction_time_ms=R(0, 3000), margin_mm=R(0, 5000),
                         t_warn_ms=R(0, 20000), t_crit_ms=R(0, 20000), min_confidence_u8=R(0, 255),
                         max_sensor_age_ms=R(0, 2000), level_medium_q15=R(0, 32767),
                         level_high_q15=R(0, 32767), level_critical_q15=R(0, 32767),
                         v_max_mm_s=R(0, 30000), v_restricted_mm_s=R(0, 30000),
                         v_crawl_mm_s=R(0, 30000), v_invalid_mm_s=R(0, 30000))


rows = [",".join(rm.INPUT_FIELDS + rm.CONFIG_FIELDS + rm.OUTPUT_FIELDS)]
for i in range(n):
    wide = i % 3 == 0
    inp = rm.RiskInput(
        distance_mm=random.getrandbits(32) if wide else R(0, 250000),
        closing_speed_mm_s=random.getrandbits(32) - 2**31 if wide else R(-25000, 25000),
        ego_speed_mm_s=random.getrandbits(32) if wide else R(0, 25000),
        sensor_age_ms=random.getrandbits(16) if wide else R(0, 500),
        confidence_u8=R(0, 255))
    c = cfg() if i % 2 else rm.RiskConfig()
    o = rm.risk_eval(inp, c)
    d = {**dataclasses.asdict(inp), **dataclasses.asdict(c), **dataclasses.asdict(o)}
    rows.append(",".join(str(d[k]) for k in rm.INPUT_FIELDS + rm.CONFIG_FIELDS + rm.OUTPUT_FIELDS))
Path(out).write_text("\n".join(rows) + "\n")
