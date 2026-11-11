# RACE kernel HLS results (Vitis HLS 2025.1, part xc7a100tcsg324-1 Artix-7, target clock 5 ns)

C simulation, C synthesis and C/RTL co-simulation pass for every version below
(co-simulation: 496 vectors, 0 mismatches against the golden model).

| Version | Change | Loop II | Batch latency (cycles, 1..256 samples) | Est. clock | LUT | FF | DSP | BRAM18 |
|---|---|---|---|---|---|---|---|---|
| v1 | `risk_eval(in[i], cfg)` direct | 5 | 215 .. 1490 | 4.583 ns | 27,621 (43%) | 36,228 (28%) | 21 | 8 |
| v2 | local record copy + `aggregate` pragmas | 2 | 150 .. 660 | 4.583 ns | 25,253 (39%) | 38,598 (30%) | 20 | 8 |

v1 cause of II=5: one AXI read per struct field through a single port (HLS 200-885).
v2 remaining violation: the 16-byte input record is read as two 64-bit bus beats (HLS 200-880).
Raw v1 numbers are from the first synthesis run; v2 reports are in `risk_v2/`.

## v3: 128/256-bit bus words (clock target 5 ns)
- Loop II = 1 (v1: 5, v2: 2). Cosim PASS, 496 vectors, 0 errors.
- Latency 121-376 cycles for 1-256 samples.
- 22,219 LUT (35%), 39,477 FF (31%), 20 DSP, 8 BRAM_18K on xc7a100t.
- Estimated clock 6.60 ns against the 5 ns target, so timing is not met. Next step: re-run at 8 ns.

## v3 at 8 ns target (final)
- Loop II = 1, cosim PASS (496 vectors, 0 errors).
- Estimated clock 6.603 ns vs 8.00 ns target: met on HLS estimate (no place-and-route run).
- Latency 114-369 cycles for 1-256 samples.
- 21,213 LUT (33%), 35,743 FF (28%), 20 DSP, 8 BRAM_18K on xc7a100t.
