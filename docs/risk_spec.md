# RACE risk engine: bit-exact specification v1

RACE (Risk Assessment and Collision Envelope) decides how fast a robot may move, given one obstacle measurement. This document is the contract. Three implementations must agree with it bit for bit:

| Implementation | File |
|---|---|
| Python golden model (arbitrary-precision ints) | `accelerator/reference/risk_model.py` |
| Portable C++ core (QNX `riskd` and HLS share it) | `accelerator/src/risk_core.cpp` |
| Vitis HLS kernel (batch wrapper around the core) | `accelerator/src/risk_kernel.cpp` |

No floating point anywhere. All divisions are unsigned floor divisions. All intermediate values fit the stated C++ type (the golden model asserts this on every vector).

## Inputs

| Field | Type | Unit | Valid range |
|---|---|---|---|
| `distance_mm` | uint32 | mm | 0 .. 200000 |
| `closing_speed_mm_s` | int32 | mm/s | -20000 .. 20000 (positive = obstacle getting closer) |
| `ego_speed_mm_s` | uint32 | mm/s | 0 .. 20000 |
| `sensor_age_ms` | uint16 | ms | any |
| `confidence_u8` | uint8 | 0..255 = 0..100 % | any |

## Configuration

| Field | Type | Meaning |
|---|---|---|
| `decel_mm_s2` | uint32 | guaranteed braking deceleration, 1 .. 20000 |
| `reaction_time_ms` | uint16 | time before braking starts |
| `margin_mm` | uint16 | extra clearance to keep |
| `t_warn_ms`, `t_crit_ms` | uint16 | time-to-collision band, `t_warn > t_crit` |
| `min_confidence_u8` | uint8 | below this the frame is rejected |
| `max_sensor_age_ms` | uint16 | older than this the frame is rejected |
| `level_medium_q15`, `level_high_q15`, `level_critical_q15` | uint16 | risk thresholds, `medium < high < critical <= 32767` |
| `v_max_mm_s`, `v_restricted_mm_s`, `v_crawl_mm_s` | uint32 | speed limit for LOW, MEDIUM, HIGH |
| `v_invalid_mm_s` | uint32 | speed limit when the input is rejected |

## Outputs

`risk_q15` (uint16, 0..32767), `risk_level` (uint16), `time_to_collision_ms` (uint32), `stopping_distance_mm` (uint32), `max_safe_speed_mm_s` (uint32), `status` (uint16).

`status`: 0 VALID, 1 STALE_SENSOR, 2 LOW_CONFIDENCE, 3 INVALID_INPUT, 4 COMPUTATION_ERROR (bad configuration).
`risk_level`: 0 LOW, 1 MEDIUM, 2 HIGH, 3 CRITICAL, 4 INVALID.

## Evaluation order

1. **Configuration check.** If any rule in the configuration table is violated: `COMPUTATION_ERROR`, speed limit 0.
2. **Input range check.** Any input outside its valid range: `INVALID_INPUT`, speed limit `v_invalid`.
3. `sensor_age_ms > max_sensor_age_ms`: `STALE_SENSOR`, speed limit `v_invalid`.
4. `confidence_u8 < min_confidence_u8`: `LOW_CONFIDENCE`, speed limit `v_invalid`.

Steps 1 to 4 return the fail-safe output: `risk_q15 = 32767`, `risk_level = 4`, `time_to_collision_ms = 0`, `stopping_distance_mm = 0`, and the speed limit above. A rejected frame never looks safe.

5. Otherwise compute, with `d = distance_mm`, `ve = ego_speed_mm_s`, `vc = closing_speed_mm_s`, `a = decel_mm_s2`, `Tr = reaction_time_ms`:

```
ttc    = vc > 0 ? min(1000*d / vc, 60000) : 60000          (uint32, 1000*d <= 2e8)
d_stop = ve*ve / (2*a)                                       (uint32, ve*ve <= 4e8)
d_rea  = ve*Tr / 1000                                        (uint32, <= 1310700)
stopping_distance_mm = d_stop + d_rea
d_req  = stopping_distance_mm + margin_mm

R_d = d_req > d ? min(((d_req - d) << 15) / d_req, 32767) : 0                  (uint64 numerator)
R_t = (vc > 0 and ttc < t_warn) ? min(((t_warn - ttc) << 15) / (t_warn - t_crit), 32767) : 0   (uint32)
risk_q15 = max(R_d, R_t)

level = risk >= level_critical ? 3 : risk >= level_high ? 2 : risk >= level_medium ? 1 : 0
limit = [v_max, v_restricted, v_crawl, 0][level]

m      = d > margin_mm ? d - margin_mm : 0
B      = (a*Tr + 999) / 1000                                 (ceiling, so the result is conservative)
S      = B*B + 2*a*m                                         (uint64, <= 1.8e12)
v_dist = m > 0 ? isqrt(S) - B : 0                            (isqrt = floor square root)
max_safe_speed_mm_s = min(limit, v_dist)
```

`v_dist` is the highest speed from which the robot can still stop inside the free distance `m`. It solves `v^2/(2a) + v*Tr/1000 <= m`. Because `B` is rounded up, `max_safe_speed_mm_s` never allows a stop that needs more distance than is available (tested as a property).

## Known simplifications

- One obstacle, straight-line approach, constant deceleration. This is a research prototype, not a certified safety function.
- `ttc` is capped at 60000 ms, so a slowly closing or static scene reports 60000 rather than infinity. If `t_warn_ms` is configured above 60000, a very slow approach still produces a small `R_t`.
