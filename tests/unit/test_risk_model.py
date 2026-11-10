"""Properties of the RACE golden model, plus consistency of the committed vectors."""

import csv
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "accelerator" / "reference"))
sys.path.insert(0, str(ROOT / "accelerator" / "testbench"))

import generate_vectors as gv  # noqa: E402
import risk_model as rm  # noqa: E402

CFG = rm.RiskConfig()


def ev(d=5000, vc=800, ve=800, age=10, conf=245, cfg=CFG):
    return rm.risk_eval(rm.RiskInput(d, vc, ve, age, conf), cfg)


def test_nominal_far_obstacle_is_low_risk_full_speed():
    o = ev(8000, 500, 500)
    assert (o.status, o.risk_level, o.max_safe_speed_mm_s) == (rm.VALID, rm.LEVEL_LOW, CFG.v_max_mm_s)


def test_obstacle_inside_stopping_distance_is_critical_and_stops():
    o = ev(300, 800, 800)
    assert o.risk_level == rm.LEVEL_CRITICAL and o.max_safe_speed_mm_s == 0


@pytest.mark.parametrize(
    "kwargs, status",
    [
        (dict(d=200001), rm.INVALID_INPUT),
        (dict(vc=20001), rm.INVALID_INPUT),
        (dict(vc=-(2**31)), rm.INVALID_INPUT),
        (dict(ve=2**32 - 1), rm.INVALID_INPUT),
        (dict(age=101), rm.STALE_SENSOR),
        (dict(conf=127), rm.LOW_CONFIDENCE),
        (dict(d=200001, age=500, conf=0), rm.INVALID_INPUT),
        (dict(age=500, conf=0), rm.STALE_SENSOR),
    ],
)
def test_rejected_frames_fail_safe_never_look_safe(kwargs, status):
    o = ev(**kwargs)
    assert o.status == status
    assert (o.risk_q15, o.risk_level) == (rm.Q15_MAX, rm.LEVEL_INVALID)
    assert o.max_safe_speed_mm_s == CFG.v_invalid_mm_s


@pytest.mark.parametrize(
    "bad",
    [
        rm.RiskConfig(decel_mm_s2=0),
        rm.RiskConfig(decel_mm_s2=20001),
        rm.RiskConfig(t_warn_ms=1000, t_crit_ms=1000),
        rm.RiskConfig(level_medium_q15=9000, level_high_q15=9000),
        rm.RiskConfig(level_critical_q15=32768),
    ],
)
def test_bad_config_is_computation_error_with_zero_speed(bad):
    o = ev(cfg=bad)
    assert o.status == rm.COMPUTATION_ERROR and o.max_safe_speed_mm_s == 0 and o.risk_level == rm.LEVEL_INVALID


def test_risk_never_decreases_as_obstacle_gets_closer():
    rng = gv.Rng(7)
    for _ in range(300):
        ve = rng.randint(0, 3000)
        vc = rng.randint(1, 3000)
        prev = None
        for d in range(8000, -1, -50):
            r = ev(d, vc, ve).risk_q15
            assert prev is None or r >= prev
            prev = r


def test_max_safe_speed_always_fits_in_the_free_distance():
    """The headline safety property: obeying max_safe_speed, the robot can stop in time."""
    rng = gv.Rng(11)
    checked = 0
    for _ in range(20000):
        c = gv.random_cfg_valid(rng, wide=rng.chance(50))
        i = rm.RiskInput(rng.randint(0, 200000), rng.randint(-20000, 20000), rng.randint(0, 20000), 0, 255)
        c = rm.RiskConfig(**{**c.__dict__, "min_confidence_u8": 0, "max_sensor_age_ms": 65535})
        o = rm.risk_eval(i, c)
        assert o.status == rm.VALID
        v = min(o.max_safe_speed_mm_s, 20000)  # speeds above the input range are not stoppable by this model
        if o.max_safe_speed_mm_s <= 20000 and v > 0:
            need = (v * v) // (2 * c.decel_mm_s2) + (v * c.reaction_time_ms) // 1000 + c.margin_mm
            assert need <= i.distance_mm, (i, c, o)
            checked += 1
        if i.distance_mm <= c.margin_mm:
            assert o.max_safe_speed_mm_s == 0
    assert checked > 1000


def test_vector_generation_is_deterministic_and_committed_files_are_in_sync(tmp_path):
    out = tmp_path / "v"
    subprocess.run([sys.executable, str(ROOT / "accelerator/testbench/generate_vectors.py"), "--out", str(out), "--random", "50"], check=True)
    assert (out / "directed.csv").read_bytes() == (ROOT / "accelerator/vectors/directed.csv").read_bytes()


def test_directed_vectors_match_the_model():
    with (ROOT / "accelerator/vectors/directed.csv").open() as f:
        for r in csv.DictReader(f):
            i = rm.RiskInput(*(int(r[k]) for k in rm.INPUT_FIELDS))
            c = rm.RiskConfig(**{k: int(r[k]) for k in rm.CONFIG_FIELDS})
            want = rm.RiskOutput(*(int(r[k]) for k in rm.OUTPUT_FIELDS))
            assert rm.risk_eval(i, c) == want
