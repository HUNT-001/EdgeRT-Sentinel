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
