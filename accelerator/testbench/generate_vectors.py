#!/usr/bin/env python3
"""Generate RACE test vectors from the golden model.

Writes, into --out (default accelerator/vectors):
  directed.csv         hand-picked boundary cases (committed)
  random_seeded.csv    seeded random cases, 10000 by default (generated, not committed)
  embedded_vectors.h   directed + first 400 random rows as a C array, so the Vitis
                       csim/cosim testbench needs no file access (committed)

Column order: inputs, configuration, expected outputs (see risk_model.py).
The random stream uses SplitMix64, so output is identical on every Python version.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "reference"))
import risk_model as rm  # noqa: E402

COLUMNS = rm.INPUT_FIELDS + rm.CONFIG_FIELDS + rm.OUTPUT_FIELDS
EMBEDDED_RANDOM_ROWS = 400
MASK64 = (1 << 64) - 1


class Rng:
    """SplitMix64: tiny, deterministic, version-independent."""

    def __init__(self, seed: int):
        self.state = seed & MASK64

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)

    def randint(self, lo: int, hi: int) -> int:
        return lo + self.next() % (hi - lo + 1)

    def chance(self, percent: int) -> bool:
        return self.randint(0, 99) < percent


def inp(d=5000, vc=800, ve=800, age=10, conf=245) -> rm.RiskInput:
    return rm.RiskInput(d, vc, ve, age, conf)


def cfg(**kw) -> rm.RiskConfig:
    return rm.RiskConfig(**kw)


def row(i: rm.RiskInput, c: rm.RiskConfig) -> list[int]:
    o = rm.risk_eval(i, c)
    return (
        [getattr(i, f) for f in rm.INPUT_FIELDS]
        + [getattr(c, f) for f in rm.CONFIG_FIELDS]
        + [getattr(o, f) for f in rm.OUTPUT_FIELDS]
    )


def level_boundaries(c: rm.RiskConfig) -> list[int]:
    """Distances (ve=0, vc=1000, so ttc == d) where risk_level changes."""
    out, prev = [], None
    for d in range(c.margin_mm, 5000):
        lvl = rm.risk_eval(inp(d, 1000, 0), c).risk_level
        if prev is not None and lvl != prev:
            out += [d - 1, d, d + 1]
        prev = lvl
    return out


def directed() -> list[list[int]]:
    c = cfg()
    cases: list[tuple[rm.RiskInput, rm.RiskConfig]] = []
    add = lambda i, k=c: cases.append((i, k))  # noqa: E731

    # nominal, stationary, receding, critical, touching
    add(inp(8000, 500, 500))
    add(inp(1000, 0, 0))
    add(inp(2000, -300, 300))
    add(inp(300, 800, 800))
    add(inp(0, 800, 800))
    add(inp(0, 0, 0))
    # margin boundary with ve=0 (d_req == margin)
    for d in (c.margin_mm - 1, c.margin_mm, c.margin_mm + 1):
        add(inp(d, 0, 0))
    # time-to-collision band edges (vc=1000 so ttc == d)
    for d in (c.t_crit_ms - 1, c.t_crit_ms, c.t_crit_ms + 1, c.t_warn_ms - 1, c.t_warn_ms, c.t_warn_ms + 1):
        add(inp(d, 1000, 0))
    # ttc cap and zero
    add(inp(200000, 1, 0))
    add(inp(60001, 1, 0))
    add(inp(0, 20000, 0))
    # risk level boundaries found by scanning the model
    for d in level_boundaries(c):
        add(inp(d, 1000, 0))
    # risk exactly equal to each level threshold (>= vs > matters)
    for d in (2500, 2200, 1500, 1000, 700):
        r = rm.risk_eval(inp(d, 1000, 0), c).risk_q15
        if 3 <= r <= 32765:
            add(inp(d, 1000, 0), cfg(level_medium_q15=r, level_high_q15=r + 1, level_critical_q15=r + 2))
            add(inp(d, 1000, 0), cfg(level_medium_q15=r - 2, level_high_q15=r, level_critical_q15=r + 1))
            add(inp(d, 1000, 0), cfg(level_medium_q15=r - 2, level_high_q15=r - 1, level_critical_q15=r))
    # non-round arithmetic: floor and ceiling rounding in d_rea, B, ttc, R_d, R_t
    for ve, tr, a, d in ((777, 333, 7, 5000), (1234, 999, 1501, 3000), (19999, 1, 3, 150000), (801, 17, 1999, 900), (3, 0, 5, 1000), (7, 0, 25, 1000), (57, 0, 65, 2000)):
        add(inp(d, ve, ve), cfg(decel_mm_s2=a, reaction_time_ms=tr))
    for d, vc in ((2999, 7), (1001, 3), (10, 19999), (12345, 4321)):
        add(inp(d, vc, vc), cfg(t_warn_ms=60000, t_crit_ms=7))
    # input range edges: valid and just outside
    for d in (200000, 200001, 2**32 - 1):
        add(inp(d, 800, 800))
    for vc in (20000, 20001, -20000, -20001, 2**31 - 1, -(2**31)):
        add(inp(5000, vc, 800))
    for ve in (20000, 20001, 2**32 - 1):
        add(inp(5000, 800, ve))
    # sensor age and confidence edges, and rejection priority
    for age in (100, 101, 65535):
        add(inp(age=age))
    for conf in (0, 127, 128, 255):
        add(inp(conf=conf))
    add(inp(d=200001, age=500, conf=0))  # invalid input beats stale and low confidence
    add(inp(age=500, conf=0))  # stale beats low confidence
    # configuration errors
    for bad in (
        cfg(decel_mm_s2=0),
        cfg(decel_mm_s2=20001),
        cfg(decel_mm_s2=2**32 - 1),
        cfg(t_warn_ms=1000, t_crit_ms=1000),
        cfg(t_warn_ms=500, t_crit_ms=1000),
        cfg(level_medium_q15=8192, level_high_q15=8192),
        cfg(level_medium_q15=20000, level_high_q15=8192),
        cfg(level_critical_q15=32768),
        cfg(level_critical_q15=65535),
    ):
        add(inp(), bad)
    # widest legal values: overflow stress
    wide = cfg(decel_mm_s2=1, reaction_time_ms=65535, margin_mm=65535, t_warn_ms=65535, t_crit_ms=0)
    fast = cfg(decel_mm_s2=20000, reaction_time_ms=65535, margin_mm=65535)
    for d in (0, 65535, 65536, 100000, 200000):
        add(inp(d, 20000, 20000), wide)
        add(inp(d, 20000, 20000), fast)
        add(inp(d, 1, 20000), wide)
    add(inp(200000, 20000, 0), fast)
    add(inp(200000, -20000, 20000), fast)
    # alternative speed limits and thresholds
    add(inp(4000, 600, 600), cfg(v_max_mm_s=2**32 - 1, v_invalid_mm_s=2**32 - 1))
    add(inp(4000, 600, 600), cfg(decel_mm_s2=20000, reaction_time_ms=0, margin_mm=0))

    return [row(i, k) for i, k in cases]


def random_cfg_valid(rng: Rng, wide: bool) -> rm.RiskConfig:
    if wide:
        t_warn = rng.randint(1, 65535)
        t_crit = rng.randint(0, t_warn - 1)
        levels = sorted({rng.randint(1, 32767) for _ in range(6)})[:3]
        while len(levels) < 3:
            levels = sorted(set(levels) | {rng.randint(1, 32767)})[:3]
        return cfg(
            decel_mm_s2=rng.randint(1, 20000),
            reaction_time_ms=rng.randint(0, 65535),
            margin_mm=rng.randint(0, 65535),
            t_warn_ms=t_warn,
            t_crit_ms=t_crit,
            min_confidence_u8=rng.randint(0, 255),
            max_sensor_age_ms=rng.randint(0, 65535),
            level_medium_q15=levels[0],
            level_high_q15=levels[1],
            level_critical_q15=levels[2],
            v_max_mm_s=rng.randint(0, 2**32 - 1),
            v_restricted_mm_s=rng.randint(0, 2**32 - 1),
            v_crawl_mm_s=rng.randint(0, 2**32 - 1),
            v_invalid_mm_s=rng.randint(0, 2**32 - 1),
        )
    t_warn = rng.randint(1500, 6000)
    return cfg(
        decel_mm_s2=rng.randint(500, 5000),
        reaction_time_ms=rng.randint(20, 400),
        margin_mm=rng.randint(0, 1000),
        t_warn_ms=t_warn,
        t_crit_ms=rng.randint(200, t_warn - 200),
    )


def random_cfg_garbage(rng: Rng) -> rm.RiskConfig:
    return cfg(
        decel_mm_s2=rng.randint(0, 2**32 - 1) if rng.chance(50) else rng.randint(0, 30000),
        reaction_time_ms=rng.randint(0, 65535),
        margin_mm=rng.randint(0, 65535),
        t_warn_ms=rng.randint(0, 65535),
        t_crit_ms=rng.randint(0, 65535),
        min_confidence_u8=rng.randint(0, 255),
        max_sensor_age_ms=rng.randint(0, 65535),
        level_medium_q15=rng.randint(0, 65535),
        level_high_q15=rng.randint(0, 65535),
        level_critical_q15=rng.randint(0, 65535),
        v_max_mm_s=rng.randint(0, 2**32 - 1),
        v_restricted_mm_s=rng.randint(0, 2**32 - 1),
        v_crawl_mm_s=rng.randint(0, 2**32 - 1),
        v_invalid_mm_s=rng.randint(0, 2**32 - 1),
    )


def random_row(rng: Rng, last_cfg: rm.RiskConfig) -> tuple[list[int], rm.RiskConfig]:
    kind = rng.randint(0, 99)
    if kind < 65:  # plausible operating point, mostly the default configuration
        ve = rng.randint(0, 2500)
        vc = max(-20000, min(20000, ve + rng.randint(-300, 300)))
        i = inp(rng.randint(0, 6000), vc, ve, rng.randint(0, 150), rng.randint(90, 255))
        c = cfg() if rng.chance(70) else random_cfg_valid(rng, wide=False)
    elif kind < 90:  # whole valid range, wide configuration
        c = last_cfg if rng.chance(30) else random_cfg_valid(rng, wide=True)
        age = rng.randint(0, c.max_sensor_age_ms) if rng.chance(85) else rng.randint(0, 65535)
        conf = rng.randint(c.min_confidence_u8, 255) if rng.chance(85) else rng.randint(0, 255)
        i = inp(rng.randint(0, 200000), rng.randint(-20000, 20000), rng.randint(0, 20000), age, conf)
    else:  # garbage everywhere
        i = inp(
            rng.randint(0, 2**32 - 1),
            rng.randint(-(2**31), 2**31 - 1),
            rng.randint(0, 2**32 - 1),
            rng.randint(0, 65535),
            rng.randint(0, 255),
        )
        c = random_cfg_garbage(rng) if rng.chance(35) else cfg()
    return row(i, c), c


def random_rows(n: int, seed: int) -> list[list[int]]:
    rng, last, rows = Rng(seed), cfg(), []
    for _ in range(n):
        r, last = random_row(rng, last)
        rows.append(r)
    return rows


def write_csv(path: Path, rows: list[list[int]]) -> None:
    with path.open("w", newline="\n") as f:
        f.write(",".join(COLUMNS) + "\n")
        for r in rows:
            f.write(",".join(map(str, r)) + "\n")


def write_header(path: Path, rows: list[list[int]]) -> None:
    with path.open("w", newline="\n") as f:
        f.write("// Generated by accelerator/testbench/generate_vectors.py. Do not edit.\n")
        f.write(f"// Columns: {', '.join(COLUMNS)}\n")
        f.write("#pragma once\n\n")
        f.write(f"static const int kEmbeddedCols = {len(COLUMNS)};\n")
        f.write(f"static const int kEmbeddedRows = {len(rows)};\n")
        f.write(f"static const long long kEmbeddedVectors[{len(rows)}][{len(COLUMNS)}] = {{\n")
        for r in rows:
            f.write("    {" + ", ".join(f"{v}LL" for v in r) + "},\n")
        f.write("};\n")


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=here.parent / "vectors")
    ap.add_argument("--random", type=int, default=10000, help="number of random rows")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0xED6E5EED)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    d_rows = directed()
    r_rows = random_rows(args.random, args.seed)
    write_csv(args.out / "directed.csv", d_rows)
    write_csv(args.out / "random_seeded.csv", r_rows)
    write_header(args.out / "embedded_vectors.h", d_rows + r_rows[:EMBEDDED_RANDOM_ROWS])
    print(f"directed={len(d_rows)} random={len(r_rows)} embedded={len(d_rows) + min(EMBEDDED_RANDOM_ROWS, len(r_rows))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
