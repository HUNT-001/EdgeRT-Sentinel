import json
from pathlib import Path

import pytest
from typer.testing import CliRunner

from edgert import scenario as sc
from edgert.cli import app

SCEN = sc.repo_root() / "scenarios"
GOOD = dict(name="t", rate_hz=100, duration_ms=1000,
            keyframes=[{"t": 0, "distance_mm": 5000, "ego_speed_mm_s": 500}])


def test_all_shipped_scenarios_validate_and_expand():
    files = sorted(SCEN.glob("*.yaml"))
    assert len(files) >= 5
    for p in files:
        frames = sc.expand(sc.load(p))
        assert frames and all(f["expected"]["status"] in range(5) for f in frames)


def test_expansion_is_deterministic():
    p = SCEN / "approach_stop.yaml"
    assert sc.expand(sc.load(p)) == sc.expand(sc.load(p))


def test_approach_stop_escalates_then_relaxes_once_slowed():
    lv = [f["expected"]["risk_level"] for f in sc.expand(sc.load(SCEN / "approach_stop.yaml"))]
    peak = lv.index(max(lv))
    assert lv[0] == 0 and max(lv) == 3
    assert lv[:peak + 1] == sorted(lv[:peak + 1])  # never relaxes while still closing fast
    assert lv[-1] == 0  # stopped with margin: no residual risk


def test_sudden_obstacle_is_critical_on_first_close_frame():
    fr = sc.expand(sc.load(SCEN / "sudden_obstacle.yaml"))
    hit = next(f for f in fr if f["in"]["obstacle_distance_mm"] == 1200)
    assert hit["expected"]["risk_level"] == 3


def test_stale_window_fails_safe_and_recovers():
    fr = sc.expand(sc.load(SCEN / "sensor_stale.yaml"))
    for f in fr:
        stale = 1500 <= f["t_ms"] < 2500
        assert (f["expected"]["status"] != 0) == stale


def test_dropout_removes_frames_and_keeps_sequence_gap():
    fr = sc.expand(sc.load(SCEN / "dropout_recovery.yaml"))
    ts = [f["t_ms"] for f in fr]
    assert not any(1000 <= t < 1600 for t in ts)
    seqs = [f["seq"] for f in fr]
    assert seqs == sorted(set(seqs)) and seqs[-1] - seqs[0] + 1 > len(seqs)


def test_invalid_flag_is_rejected_by_core():
    fr = sc.expand(sc.load(SCEN / "invalid_frame.yaml"))
    bad = [f for f in fr if f["flags"] & 1]
    assert bad and all(f["expected"]["status"] == 3 for f in bad)


@pytest.mark.parametrize("mut", [
    lambda s: s.pop("name"),
    lambda s: s.update(rate_hz=0),
    lambda s: s.update(rate_hz=True),
    lambda s: s.update(bogus=1),
    lambda s: s["keyframes"][0].update(t=5),
    lambda s: s["keyframes"][0].update(distance_mm=-1),
    lambda s: s["keyframes"].append({"t": 0, "distance_mm": 1, "ego_speed_mm_s": 1}),
    lambda s: s.update(closing="fast"),
    lambda s: s.update(faults=[{"type": "nope", "from": 0, "to": 5}]),
    lambda s: s.update(faults=[{"type": "stale", "from": 0, "to": 5}]),
    lambda s: s.update(config={"not_a_field": 1}),
])
def test_invalid_scenarios_rejected(mut):
    import copy
    s = copy.deepcopy(GOOD)
    mut(s)
    with pytest.raises(sc.ScenarioError):
        sc.validate(s)


def test_cli_validate_expand_and_error_exit(tmp_path):
    r = CliRunner()
    p = str(SCEN / "approach_stop.yaml")
    assert r.invoke(app, ["scenario", "validate", p]).exit_code == 0
    out = tmp_path / "f.jsonl"
    assert r.invoke(app, ["scenario", "expand", p, "--out", str(out)]).exit_code == 0
    rows = [json.loads(l) for l in out.read_text().splitlines()]
    assert rows[0]["seq"] == 0 and "expected" in rows[0]
    bad = tmp_path / "bad.yaml"
    bad.write_text("name: x\n")
    assert r.invoke(app, ["scenario", "validate", str(bad)]).exit_code == 2
