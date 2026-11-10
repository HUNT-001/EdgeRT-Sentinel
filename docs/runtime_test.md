# Runtime IPC test (Nov 12-16 milestone)

Proves: riskd runs as its own process, native QNX message passing carries protocol v1, results are
bit-identical to the golden model across all scenarios, bad messages get fail-safe replies, and
round-trip latency is measured on the real target.

## Host (logic only, any Linux)
    pip install -e "host[dev]" && bash scripts/ipc_test_host.sh

## QNX target
    bash scripts/build_qnx.sh            # builds gate1 + runtime
    bash scripts/bake_runtime.sh         # puts binaries + scenario CSVs in the image, rebuilds it
    bash scripts/run_qemu.sh             # boot; on the QNX console:

    cd /data/home/root
    riskd --prio 50 &
    for f in *.csv; do echo $f; ipc_test --csv $f --prio 40; done
    slay riskd

Pass = `IPC TEST: PASS` for every scenario. Record the `rtt_us p50/p99/max` lines in
`docs/results/runtime/` as evidence; they are the first real IPC latency numbers for the control path.

## Full pipeline (Nov 12-16 milestone, part 2)

Six processes: `sensor_replayd` -> `motiond` (50 Hz) -> `riskd`, `actuatord`; `safetyd` watches motiond's heartbeat.
`stack_run.sh` launches them all, replays a scenario, stops them and prints every log.

Host (logic): `cd host && python3 -m pytest -q tests/test_stack.py`

QNX console (after `bake_runtime.sh` and boot):

    cd /data/home/root
    export LOGDIR=/data/home/root/stack
    RT=1 sh ./stack_run.sh . approach_stop.csv                      # normal run, real priorities
    RT=1 sh ./stack_run.sh . invalid_frame.csv --crash-after 40     # kill -9 motiond mid-run
    RT=1 sh ./stack_run.sh . invalid_frame.csv --wedge-after 40     # motiond alive but frozen

What to look for:
- `MOTION summary ... misses=0 ... max_late_us=<small>`: control-loop release jitter and deadline misses on QNX.
- Crash/wedge runs: a `MOTION FAULT` line, then `SAFETYD trip ... reason=6`, then `ACT ... applied=0 src=safetyd`
  within ~100 ms of the fault. Last `applied` must be 0 and nothing non-zero after it.
- After a scenario ends the sensor goes silent, so a final SAFE_STOP (reason=1) is expected and correct.
Save the three outputs under `docs/results/runtime/`.
