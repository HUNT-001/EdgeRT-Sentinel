# EdgeRT Sentinel

A real-time robotic safety architecture that runs natively on QNX 8.0, with an FPGA-aware risk-computation engine and a host-side workbench for fault-injection experiments.

> Status: **Gate 1 (environment feasibility).** See [docs/gate1.md](docs/gate1.md).

## Layout

```
qnx/gate1/          rt_probe (pulse timer + SCHED_FIFO), link_probe (TCP/serial telemetry)
accelerator/smoke/  Vitis HLS smoke kernel, self-checking testbench, csim/synth/cosim scripts
host/               edgert CLI: doctor, probe
scripts/            build_qnx.sh, run_qemu.sh, deploy_and_probe.sh
docs/               gate runbooks and results
```

## Quick start (Ubuntu VM)

```bash
source ~/qnx800/qnxsdp-env.sh
python3 -m venv .venv && . .venv/bin/activate && pip install -e "host[dev]"
edgert doctor --scope qnx
bash scripts/run_qemu.sh            # shell 1
bash scripts/build_qnx.sh           # shell 2
bash scripts/deploy_and_probe.sh
```

Off-target checks (no QNX or Vitis needed):

```bash
make -C qnx/gate1 HOST=1
bash accelerator/smoke/run_hls.sh --gcc
cd host && pytest
```

## Scope note

This is a simulated research prototype. It is not certified, and it is not suitable for controlling a real robot. Timing results measured in QEMU describe the test environment only.

## Licence

MIT. See [LICENSE](LICENSE). The QNX SDP, target images and AMD tools are not part of this repository.

## Off-target tests

`bash scripts/run_tests.sh` builds and runs: IPC protocol contract, RACE C++ core vs Python golden model
(directed + 10,000 seeded random vectors), golden-model property tests, and scenario/CLI tests.
Scenarios live in `scenarios/*.yaml`; try `edgert scenario summary scenarios/approach_stop.yaml`.
Specs: `docs/risk_spec.md`, `docs/ipc_protocol.md`.
