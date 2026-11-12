"""Full pipeline on the host (Unix-socket transport): sensor_replayd -> motiond -> riskd -> actuatord,
with safetyd watching. Checks behaviour and fault reactions. Logic only: real-time numbers come from QNX."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

from edgert import scenario as sc

ROOT = sc.repo_root()
BIN = ROOT / "qnx" / "build" / "host"
pytestmark = pytest.mark.skipif(shutil.which("g++") is None, reason="needs g++")

ACT = re.compile(r"ACT t_ms=(\d+) applied=(-?\d+) src=(\w+) reason=(\d+)")
MOT = re.compile(r"MOTION t_ms=(\d+) cycle=(\d+) state=(\d) reason=(\d+) speed=(\d+)")
FAULT = re.compile(r"MOTION FAULT (\w+) t_ms=(\d+) cycle=(\d+)")
TRIP = re.compile(r"SAFETYD trip t_ms=(\d+) gap_ms=(\d+) reason=(\d+)")
SUMMARY = re.compile(r"MOTION summary cycles=(\d+) misses=(\d+) late=(\d+) max_late_us=(\d+) max_exec_us=(\d+) "
                     r"riskd_err=(\d+) act_err=(\d+)")


@pytest.fixture(scope="module", autouse=True)
def built():
    subprocess.run(["make", "-s", "-C", str(ROOT / "qnx" / "runtime"), "HOST=1"], check=True)


def run_stack(tmp_path, scenario, *motiond_args):
    csv = tmp_path / f"{scenario}.csv"
    csv.write_text("\n".join(sc.to_csv_lines(sc.expand(sc.load(ROOT / "scenarios" / f"{scenario}.yaml")))) + "\n")
    env = {"LOGDIR": str(tmp_path / "logs"), "TAIL_MS": "600", "PATH": "/usr/bin:/bin"}
    r = subprocess.run(["sh", str(ROOT / "scripts" / "stack_run.sh"), str(BIN), str(csv), *motiond_args],
                       env=env, capture_output=True, text=True, timeout=60)
    logs = {n: (tmp_path / "logs" / f"{n}.log").read_text() for n in ("actuatord", "safetyd", "motiond", "sensor")}
    return r.returncode, {
        "act": [(int(t), int(a), s, int(rs)) for t, a, s, rs in ACT.findall(logs["actuatord"])],
        "mot": [tuple(map(int, m)) for m in MOT.findall(logs["motiond"])],
        "fault": FAULT.findall(logs["motiond"]),
        "trip": [tuple(map(int, m)) for m in TRIP.findall(logs["safetyd"])],
        "summary": SUMMARY.search(logs["motiond"]),
        "raw": logs,
    }


def test_blip_degrades_recovers_then_stops_only_when_sensor_dies(tmp_path):
    rc, r = run_stack(tmp_path, "invalid_frame")
    assert rc == 0, r["raw"]["sensor"]
    states = [(s, reason) for _, _, s, reason, _ in r["mot"]]
    assert (1, 3) in states                                   # DEGRADED / RISK_INVALID during the blip
    blip = states.index((1, 3))
    assert (0, 0) in states[blip:]                            # recovered to NORMAL afterwards
    stops = [x for x in states if x[0] == 2]
    assert stops == [(2, 1)]                                  # only stop: sensor silent after the scenario ended
    assert not r["trip"]                                      # watchdog never fired: motiond stayed alive
    s = r["summary"]
    assert s and int(s.group(1)) > 50 and int(s.group(7)) == 0  # act_err == 0


def test_stale_sensor_stops_and_actuator_follows(tmp_path):
    rc, r = run_stack(tmp_path, "sensor_stale")
    assert rc == 0
    stop = next(m for m in r["mot"] if m[2] == 2)
    assert stop[3] == 1                                       # reason SENSOR_STALE
    assert r["act"][-1][1] == 0                               # actuator ended at 0
    zero_t = next(t for t, a, src, _ in r["act"] if a == 0 and t >= stop[0] - 5)
    assert zero_t - stop[0] <= 100
    assert not any(a > 0 for t, a, _, _ in r["act"] if t > zero_t)   # nothing re-applied after stop


@pytest.mark.parametrize("fault,n", [("crash", 40), ("wedge", 40)])
def test_motiond_failure_is_caught_by_independent_watchdog(tmp_path, fault, n):
    rc, r = run_stack(tmp_path, "invalid_frame", f"--{fault}-after", str(n))
    f = next(x for x in r["fault"] if x[0] == fault)
    fault_t = int(f[1])
    assert r["trip"], r["raw"]["safetyd"]
    t, gap, reason = r["trip"][0]
    assert reason == 6 and gap <= 150
    # actuator forced to zero by safetyd or by its own command timeout, soon after the fault
    zero = next((t_ for t_, a, src, _ in r["act"] if a == 0 and src in ("safetyd", "timeout") and t_ >= fault_t), None)
    assert zero is not None
    assert zero - fault_t <= 400
    assert not any(a > 0 for t_, a, _, _ in r["act"] if t_ > zero)


# ---- operator reset + telemetry -------------------------------------------------------------------------

import json
import os
import time


def _launch(tmp_path, scenario, extra_motiond=()):
    csv = tmp_path / f"{scenario}.csv"
    csv.write_text("\n".join(sc.to_csv_lines(sc.expand(sc.load(ROOT / "scenarios" / f"{scenario}.yaml")))) + "\n")
    env = {**os.environ, "LOGDIR": str(tmp_path / "logs"), "TAIL_MS": "800"}
    return subprocess.Popen(["sh", str(ROOT / "scripts" / "stack_run.sh"), str(BIN), str(csv), *extra_motiond],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def _wait_log(path, pattern, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if path.exists() and re.search(pattern, path.read_text()):
            return True
        time.sleep(0.05)
    return False


def _opctl():
    return subprocess.run([str(BIN / "opctl"), "reset"], capture_output=True, text=True, timeout=10)


def _events(tmp_path):
    return [json.loads(x) for x in (tmp_path / "logs" / "telemetry.jsonl").read_text().splitlines() if x.strip()]


def test_operator_reset_recovers_after_fault_clears(tmp_path):
    p = _launch(tmp_path, "sensor_stale")                      # stale 1.5-2.5 s -> SAFE_STOP, data healthy after
    mlog = tmp_path / "logs" / "motiond.log"
    assert _wait_log(mlog, r"state=2 reason=1")
    time.sleep(1.4)                                           # stale window over, healthy frames flowing, still latched
    r = _opctl()
    assert r.returncode == 0, r.stdout + r.stderr
    out, _ = p.communicate(timeout=60)
    log = mlog.read_text()
    assert "MOTION RESET done" in log, log
    states = [(int(a), int(b)) for a, b in re.findall(r"state=(\d) reason=(\d+)", log)]
    stop = states.index((2, 1))
    assert (1, 0) in states[stop:] and (0, 0) in states[stop:]          # DEGRADED then NORMAL again
    ev = _events(tmp_path)
    assert any(e["ev"] == "operator_reset" and e["src"] == "safety" and e["v0"] == 0 for e in ev)
    assert any(e["ev"] == "operator_reset" and e["src"] == "motion" and e["v0"] == 10 for e in ev)
    assert not any(e["ev"] == "gap" for e in ev), "telemetry lost events"
    acts = [(int(t), int(a)) for t, a in re.findall(r"ACT t_ms=(\d+) applied=(-?\d+) src=motiond", (tmp_path / "logs" / "actuatord.log").read_text())]
    assert any(a > 0 for _, a in acts[len(acts) // 2:])                  # actuator moves again after the reset


def test_operator_reset_refused_while_fault_persists(tmp_path):
    p = _launch(tmp_path, "sensor_stale")
    mlog = tmp_path / "logs" / "motiond.log"
    assert _wait_log(mlog, r"state=2 reason=1")
    r = _opctl()                                              # still inside the stale window
    assert r.returncode == 0                                  # safetyd released the actuator, controller decides
    assert _wait_log(mlog, r"MOTION RESET (done|rejected)")
    log = mlog.read_text()
    assert "MOTION RESET rejected" in log, log                # controller refused: input still unhealthy
    assert "MOTION RESET done" not in log
    p.communicate(timeout=60)


def test_operator_reset_when_not_latched_is_a_noop(tmp_path):
    p = _launch(tmp_path, "approach_stop")
    assert _wait_log(tmp_path / "logs" / "motiond.log", r"state=0 reason=0")
    r = _opctl()
    assert r.returncode == 3 and "not latched" in r.stdout, r.stdout + r.stderr
    p.communicate(timeout=60)


def test_latch_release_path_with_fake_motiond(tmp_path):
    procs = [subprocess.Popen([str(BIN / n), *a], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
             for n, a in (("actuatord", []), ("safetyd", ["--grace-ms", "4000"]))]
    try:
        r = subprocess.run([str(BIN / "latch_test")], capture_output=True, text=True, timeout=30)
        assert "LATCH TEST: PASS" in r.stdout, r.stdout
    finally:
        for p in procs:
            p.terminate()
            p.wait(timeout=5)
