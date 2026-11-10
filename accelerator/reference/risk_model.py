"""Golden model of the RACE risk engine. Implements docs/risk_spec.md.

Python integers never overflow, so every intermediate is checked against the C++
type it maps to (`_fit`). If a vector ever needed more than the C++ type holds,
generating it raises instead of silently diverging from the C++ core.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, fields

DIST_MAX_MM = 200000
SPEED_MAX_MM_S = 20000
DECEL_MAX_MM_S2 = 20000
TTC_MAX_MS = 60000
Q15_MAX = 32767

VALID, STALE_SENSOR, LOW_CONFIDENCE, INVALID_INPUT, COMPUTATION_ERROR = range(5)
LEVEL_LOW, LEVEL_MEDIUM, LEVEL_HIGH, LEVEL_CRITICAL, LEVEL_INVALID = range(5)

U8, U16, U32 = (0, 2**8 - 1), (0, 2**16 - 1), (0, 2**32 - 1)
I32 = (-(2**31), 2**31 - 1)


def _fit(value: int, bits: int, what: str) -> int:
    if not 0 <= value < 2**bits:
        raise OverflowError(f"{what}={value} does not fit uint{bits}")
    return value


@dataclass(frozen=True)
class RiskInput:
    distance_mm: int
    closing_speed_mm_s: int
    ego_speed_mm_s: int
    sensor_age_ms: int
    confidence_u8: int

    RANGES = {
        "distance_mm": U32,
        "closing_speed_mm_s": I32,
        "ego_speed_mm_s": U32,
        "sensor_age_ms": U16,
        "confidence_u8": U8,
    }


@dataclass(frozen=True)
class RiskConfig:
    decel_mm_s2: int = 1500
    reaction_time_ms: int = 100
    margin_mm: int = 300
    t_warn_ms: int = 3000
    t_crit_ms: int = 1000
    min_confidence_u8: int = 128
    max_sensor_age_ms: int = 100
    level_medium_q15: int = 8192
    level_high_q15: int = 20000
    level_critical_q15: int = 28000
    v_max_mm_s: int = 1500
    v_restricted_mm_s: int = 700
    v_crawl_mm_s: int = 250
    v_invalid_mm_s: int = 200

    RANGES = {
        "decel_mm_s2": U32,
        "reaction_time_ms": U16,
        "margin_mm": U16,
        "t_warn_ms": U16,
        "t_crit_ms": U16,
        "min_confidence_u8": U8,
        "max_sensor_age_ms": U16,
        "level_medium_q15": U16,
        "level_high_q15": U16,
        "level_critical_q15": U16,
        "v_max_mm_s": U32,
        "v_restricted_mm_s": U32,
        "v_crawl_mm_s": U32,
        "v_invalid_mm_s": U32,
    }


@dataclass(frozen=True)
class RiskOutput:
    risk_q15: int
    risk_level: int
    time_to_collision_ms: int
    stopping_distance_mm: int
    max_safe_speed_mm_s: int
    status: int


INPUT_FIELDS = [f.name for f in fields(RiskInput)]
CONFIG_FIELDS = [f.name for f in fields(RiskConfig)]
OUTPUT_FIELDS = [f.name for f in fields(RiskOutput)]


def _check_types(obj) -> None:
    for name, (lo, hi) in type(obj).RANGES.items():
        v = getattr(obj, name)
        if not lo <= v <= hi:
            raise ValueError(f"{type(obj).__name__}.{name}={v} outside its C++ type range")


def _fail_safe(status: int, speed: int) -> RiskOutput:
    return RiskOutput(Q15_MAX, LEVEL_INVALID, 0, 0, speed, status)


def _config_ok(c: RiskConfig) -> bool:
    return (
        1 <= c.decel_mm_s2 <= DECEL_MAX_MM_S2
        and c.t_warn_ms > c.t_crit_ms
        and c.level_medium_q15 < c.level_high_q15 < c.level_critical_q15 <= Q15_MAX
    )


def risk_eval(inp: RiskInput, cfg: RiskConfig) -> RiskOutput:
    _check_types(inp)
    _check_types(cfg)

    if not _config_ok(cfg):
        return _fail_safe(COMPUTATION_ERROR, 0)
    if (
        inp.distance_mm > DIST_MAX_MM
        or inp.ego_speed_mm_s > SPEED_MAX_MM_S
        or not -SPEED_MAX_MM_S <= inp.closing_speed_mm_s <= SPEED_MAX_MM_S
    ):
        return _fail_safe(INVALID_INPUT, cfg.v_invalid_mm_s)
    if inp.sensor_age_ms > cfg.max_sensor_age_ms:
        return _fail_safe(STALE_SENSOR, cfg.v_invalid_mm_s)
    if inp.confidence_u8 < cfg.min_confidence_u8:
        return _fail_safe(LOW_CONFIDENCE, cfg.v_invalid_mm_s)

    d, ve, vc = inp.distance_mm, inp.ego_speed_mm_s, inp.closing_speed_mm_s
    a, tr, margin = cfg.decel_mm_s2, cfg.reaction_time_ms, cfg.margin_mm
    t_warn, t_crit = cfg.t_warn_ms, cfg.t_crit_ms

    ttc = TTC_MAX_MS
    if vc > 0:
        ttc = min(_fit(1000 * d, 32, "1000*d") // vc, TTC_MAX_MS)

    d_stop = _fit(ve * ve, 32, "ve*ve") // _fit(2 * a, 32, "2*a")
    d_rea = _fit(ve * tr, 32, "ve*tr") // 1000
    stopping = _fit(d_stop + d_rea, 32, "stopping")
    d_req = _fit(stopping + margin, 32, "d_req")

    r_d = 0
    if d_req > d:
        r_d = min(_fit((d_req - d) << 15, 64, "r_d numerator") // d_req, Q15_MAX)
    r_t = 0
    if vc > 0 and ttc < t_warn:
        r_t = min(_fit((t_warn - ttc) << 15, 32, "r_t numerator") // (t_warn - t_crit), Q15_MAX)
    risk = max(r_d, r_t)

    if risk >= cfg.level_critical_q15:
        level, limit = LEVEL_CRITICAL, 0
    elif risk >= cfg.level_high_q15:
        level, limit = LEVEL_HIGH, cfg.v_crawl_mm_s
    elif risk >= cfg.level_medium_q15:
        level, limit = LEVEL_MEDIUM, cfg.v_restricted_mm_s
    else:
        level, limit = LEVEL_LOW, cfg.v_max_mm_s

    m = d - margin if d > margin else 0
    v_dist = 0
    if m > 0:
        b = _fit(a * tr + 999, 32, "a*tr+999") // 1000
        s = _fit(b * b + 2 * a * m, 64, "S")
        v_dist = math.isqrt(s) - b

    return RiskOutput(risk, level, ttc, stopping, min(limit, v_dist), VALID)
