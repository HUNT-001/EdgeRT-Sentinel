# Gate 1: Environment feasibility (Nov 8–11)

**Exit condition:** a C++ program built with the QNX 8.0 toolchain runs on a QNX 8.0 QEMU target, and the probe evidence is committed under `docs/results/gate1/`.

If Gate 1 slips past Nov 11, cut scope (see blueprint §12). Do not shift the whole schedule.

## Where each tool runs

| Machine | Runs | Why |
|---|---|---|
| Ubuntu 22.04.5 VM (8 GB RAM, 4 vCPU, 80 GB disk, nested virtualisation on) | QNX SDP 8.0, `mkqnximage`/QEMU, `edgert` CLI | QNX's QEMU flow targets Ubuntu 22.04/24.04 |
| Windows 11 host (or the teammate's machine) | Vitis HLS 2026.1 | Vitis needs at least 32 GB RAM, so it can't run inside the VM |

Get the repo into the VM by pushing it to GitHub and cloning it there. Shared folders break the executable bit and line endings.

## Workstream A (Pavan): QNX target

1. **Licence.** Register at qnx.com/getqnx for the free non-commercial QNX Everywhere licence.
2. **Install the SDP** (in the VM). Install QNX Software Center, then QNX SDP 8.0 to `~/qnx800`. Then:
   ```bash
   source ~/qnx800/qnxsdp-env.sh
   sudo apt install -y qemu-system-x86 openssh-client python3-venv
   ```
3. **Check the environment:**
   ```bash
   python3 -m venv .venv && . .venv/bin/activate && pip install -e "host[dev]"
   edgert doctor --scope qnx        # every check must be PASS (kvm may be WARN)
   ```
4. **Boot the target:** `bash scripts/run_qemu.sh` (first run builds the image). If `mkqnximage` rejects a flag, follow the official QNX QEMU codelab and record what you changed in `docs/qnx_setup.md`.
5. **Build:** `bash scripts/build_qnx.sh`. This produces `qnx/build/qnx-x86_64/{rt_probe,link_probe}`.
6. **Deploy and collect evidence** (in a second shell, with the target running):
   ```bash
   bash scripts/deploy_and_probe.sh      # or: TARGET_IP=<ip> bash scripts/deploy_and_probe.sh
   ```
   - `rt_probe` runs a 60 s, 50 Hz loop: `timer_create(CLOCK_MONOTONIC)` → `SIGEV_PULSE` → `MsgReceivePulse` on a `SCHED_FIFO` thread. This is motiond's release mechanism.
   - `link_probe` streams 200 JSON lines over TCP to `edgert probe`. This is telemetryd's transport.

### Serial fallback (if QEMU networking fails)
On the target console:
```sh
/tmp/edgert/link_probe --stdout --count 200 --hz 20
```
Capture the console output to a file, then on the host run:
```bash
edgert probe --log console.log --expect 200 --hz 20
```
Networking is not on the critical path. The runtime work can go ahead on serial.

## Workstream B (teammate): HLS and host

1. Install Vitis 2026.1 (HLS component, plus the device family for the part in `accelerator/smoke/hls_config.cfg`).
2. Check the environment: `edgert doctor --scope hls`.
3. Run `accelerator/smoke/run_hls.bat` (Windows) or `run_hls.sh` (Linux). It runs csim → synthesis → cosim, then copies the reports.
4. Copy `accelerator/smoke/reports/*` into `docs/results/gate1/hls/`.

## Gate 1 checklist

- [ ] `edgert doctor --scope qnx` passes in the VM
- [ ] QNX target boots in QEMU; `uname -a` reports QNX 8.0
- [ ] `GATE1 RT PROBE: PASS` from a QNX build (not the `HOST FALLBACK` build)
- [ ] `GATE1 LINK PROBE: PASS` over TCP, or over the serial fallback (record which)
- [ ] `GATE1 HLS SMOKE TB: PASS` in csim, with synthesis and cosim reports saved
- [ ] Evidence committed under `docs/results/gate1/`
- [ ] VM spec and QEMU config written in `docs/qnx_setup.md` (needed later for benchmark reporting)

## Pass criteria and what they prove

| Probe | Pass | What it shows |
|---|---|---|
| `rt_probe` | `SCHED_FIFO` settable, 0 overruns, max release jitter < 10 ms | Pulse timers, channels and RT priorities work in our QEMU image |
| `link_probe` | 200/200 lines, no gaps or reordering | Target-to-host telemetry path works |
| HLS smoke | 0 testbench mismatches in csim and cosim | Toolchain, device part and RTL co-simulation work |

Jitter numbers from QEMU are not real-time guarantees. They only establish a baseline for this setup.
