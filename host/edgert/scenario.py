"""Scenario files: describe a run once, expand it to deterministic sensor frames,
and compute the expected RACE output for every frame with the golden model.

Scenario YAML (all times in ms):

  name: str
  rate_hz: int                  # sensor frame rate, 1..1000
  duration_ms: int
  config: {field: value}        # optional RiskConfig overrides
  keyframes:                    # piecewise-linear, held after the last key
    - {t: 0, distance_mm: 9000, ego_speed_mm_s: 1000}
  closing: derived | {constant: int}   # default derived = -d(distance)/dt
  confidence_u8: 245
  faults:                       # optional, applied after interpolation
    - {type: dropout|stale|low_confidence|invalid, from: ms, to: ms, age_ms: int}
"""

from __future__ import annotations

import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any

import yaml

FAULT_TYPES = ("dropout", "stale", "low_confidence", "invalid")
KEYS = ("name", "rate_hz", "duration_ms", "config", "keyframes", "closing", "confidence_u8", "faults",
        "description")


class ScenarioError(ValueError):
    pass


def repo_root() -> Path:
    for p in [Path(__file__).resolve(), *Path(__file__).resolve().parents]:
        if (p / "accelerator" / "reference" / "risk_model.py").exists():
            return p
    raise ScenarioError("cannot locate accelerator/reference/risk_model.py (run from a repo checkout)")


def _model():
    ref = str(repo_root() / "accelerator" / "reference")
    if ref not in sys.path:
        sys.path.insert(0, ref)
    import risk_model  # noqa: PLC0415
    return risk_model


def _int(v: Any, what: str, lo: int, hi: int) -> int:
    if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
        raise ScenarioError(f"{what} must be an integer in [{lo}, {hi}], got {v!r}")
    return v


def load(path: Path) -> dict:
    data = yaml.safe_load(Path(path).read_text(encoding="utf-8"))
    return validate(data)


def validate(s: Any) -> dict:
    if not isinstance(s, dict):
        raise ScenarioError("scenario must be a mapping")
    extra = set(s) - set(KEYS)
    if extra:
        raise ScenarioError(f"unknown keys: {sorted(extra)}")
    for k in ("name", "rate_hz", "duration_ms", "keyframes"):
        if k not in s:
            raise ScenarioError(f"missing key: {k}")
    if not isinstance(s["name"], str) or not s["name"]:
        raise ScenarioError("name must be a non-empty string")
    _int(s["rate_hz"], "rate_hz", 1, 1000)
    _int(s["duration_ms"], "duration_ms", 1, 600_000)
    s.setdefault("confidence_u8", 245)
    _int(s["confidence_u8"], "confidence_u8", 0, 255)
    kf = s["keyframes"]
    if not isinstance(kf, list) or not kf:
        raise ScenarioError("keyframes must be a non-empty list")
    last = -1
    for i, k in enumerate(kf):
        if not isinstance(k, dict) or set(k) != {"t", "distance_mm", "ego_speed_mm_s"}:
            raise ScenarioError(f"keyframes[{i}] needs exactly t, distance_mm, ego_speed_mm_s")
        t = _int(k["t"], f"keyframes[{i}].t", 0, 600_000)
        if t <= last:
            raise ScenarioError(f"keyframes[{i}].t must be strictly increasing")
        last = t
        _int(k["distance_mm"], f"keyframes[{i}].distance_mm", 0, 200_000)
        _int(k["ego_speed_mm_s"], f"keyframes[{i}].ego_speed_mm_s", 0, 20_000)
    if kf[0]["t"] != 0:
        raise ScenarioError("first keyframe must have t: 0")
    c = s.get("closing", "derived")
    if c != "derived" and not (isinstance(c, dict) and set(c) == {"constant"}):
        raise ScenarioError("closing must be 'derived' or {constant: int}")
    if isinstance(c, dict):
        _int(c["constant"], "closing.constant", -20_000, 20_000)
    for i, f in enumerate(s.get("faults", [])):
        if not isinstance(f, dict) or f.get("type") not in FAULT_TYPES:
            raise ScenarioError(f"faults[{i}].type must be one of {FAULT_TYPES}")
        _int(f.get("from"), f"faults[{i}].from", 0, 600_000)
        _int(f.get("to"), f"faults[{i}].to", 0, 600_000)
        if f["to"] <= f["from"]:
            raise ScenarioError(f"faults[{i}] needs to > from")
        if f["type"] == "stale":
            _int(f.get("age_ms"), f"faults[{i}].age_ms", 0, 65_535)
    rm = _model()
    allowed = set(rm.CONFIG_FIELDS)
    bad = set(s.get("config", {})) - allowed
    if bad:
        raise ScenarioError(f"unknown config fields: {sorted(bad)}")
    return s


def _lerp(t: int, kf: list[dict], key: str) -> int:
    if t >= kf[-1]["t"]:
        return kf[-1][key]
    for a, b in zip(kf, kf[1:]):
        if a["t"] <= t < b["t"]:
            # floor division on exact integers keeps expansion deterministic across platforms
            return a[key] + ((b[key] - a[key]) * (t - a["t"])) // (b["t"] - a["t"])
    return kf[0][key]


def expand(s: dict) -> list[dict]:
    """Return one dict per frame: sensor inputs plus the expected golden output."""
    rm = _model()
    cfg = rm.RiskConfig(**s.get("config", {}))
    period_us = 1_000_000 // s["rate_hz"]
    n = (s["duration_ms"] * 1000) // period_us + 1
    kf = s["keyframes"]
    closing = s.get("closing", "derived")
    frames = []
    prev_d = None
    for i in range(n):
        t_ms = (i * period_us) // 1000
        d = _lerp(t_ms, kf, "distance_mm")
        ve = _lerp(t_ms, kf, "ego_speed_mm_s")
        if isinstance(closing, dict):
            vc = closing["constant"]
        elif prev_d is None:
            vc = 0
        else:
            vc = ((prev_d - d) * 1_000_000) // period_us  # mm/s, positive when closing
        prev_d = d
        vc = max(-20_000, min(20_000, vc))
        age, conf, flags, present = 0, s["confidence_u8"], 0, True
        for f in s.get("faults", []):
            if f["from"] <= t_ms < f["to"]:
                if f["type"] == "dropout":
                    present = False
                elif f["type"] == "stale":
                    age = f["age_ms"]
                elif f["type"] == "low_confidence":
                    conf = 0
                elif f["type"] == "invalid":
                    flags |= 1
        if not present:
            continue
        dist_in = 200_001 if flags & 1 else d
        out = rm.risk_eval(rm.RiskInput(dist_in, vc, ve, age, conf), cfg)
        frames.append({"seq": i, "t_ms": t_ms, "flags": flags,
                       "in": {"obstacle_distance_mm": dist_in, "closing_speed_mm_s": vc,
                              "ego_speed_mm_s": ve, "sensor_age_ms": age, "confidence_u8": conf},
                       "expected": asdict(out)})
    return frames


def summarize(frames: list[dict]) -> dict:
    levels = [f["expected"]["risk_level"] for f in frames]
    return {"frames": len(frames),
            "max_level": max(levels) if levels else None,
            "first_t_ms_by_level": {str(l): next(f["t_ms"] for f in frames if f["expected"]["risk_level"] == l)
                                    for l in sorted(set(levels))},
            "invalid_frames": sum(1 for f in frames if f["expected"]["status"] != 0)}


CSV_HEADER = "seq,t_ms,flags,dist,closing,ego,age,conf,risk_q15,risk_level,ttc_ms,stop_mm,vmax,status"


def to_csv_lines(frames: list[dict]) -> list[str]:
    """Integer-only CSV for the target-side ipc_test (no JSON parser needed on QNX)."""
    rows = [CSV_HEADER]
    for f in frames:
        i, e = f["in"], f["expected"]
        rows.append(",".join(str(x) for x in (
            f["seq"], f["t_ms"], f["flags"], i["obstacle_distance_mm"], i["closing_speed_mm_s"],
            i["ego_speed_mm_s"], i["sensor_age_ms"], i["confidence_u8"], e["risk_q15"], e["risk_level"],
            e["time_to_collision_ms"], e["stopping_distance_mm"], e["max_safe_speed_mm_s"], e["status"])))
    return rows
