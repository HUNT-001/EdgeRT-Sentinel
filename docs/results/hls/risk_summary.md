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
