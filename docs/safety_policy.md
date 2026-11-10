# Safety policy v1

Code: `qnx/common/include/edgert_control.h` (pure logic, no OS calls). Tests: `tests/unit/test_control.cpp`
(exact-cycle transitions, latching, watchdog, and per-cycle invariants over every scenario).
All numbers are defaults in `ControllerConfig` / `MonitorConfig`, chosen for a 50 Hz loop and a
~1 m/s robot; they are engineering choices for review, not derived from a standard.

## States
`DEGRADED` (power-up) -> `NORMAL` -> `DEGRADED` -> `SAFE_STOP`. SAFE_STOP is latched.

| Condition | Result |
|---|---|
| Inputs healthy (risk response valid, data age <= 100 ms) for 25 consecutive cycles (500 ms) | DEGRADED -> NORMAL |
| Any unhealthy cycle | NORMAL -> DEGRADED, reason SENSOR_STALE or RISK_INVALID |
| 10 consecutive unhealthy cycles (200 ms) | SAFE_STOP |
| CRITICAL risk level for 5 consecutive healthy cycles (100 ms) | SAFE_STOP (CRITICAL_RISK) |
| safetyd demands stop (heartbeat lost) | SAFE_STOP immediately, overrides everything |
| Operator reset | Only if inputs healthy, risk not CRITICAL, safetyd not demanding stop; goes to DEGRADED, never straight to NORMAL |

An unhealthy cycle restarts the healthy and critical streaks.

## Commanded speed
- SAFE_STOP: 0.
- DEGRADED: min(250 mm/s, nominal, max_safe_speed if risk valid).
- NORMAL: min(nominal 1000 mm/s, max_safe_speed).
- Reductions apply immediately. Increases ramp at 10 mm/s per cycle (500 mm/s^2).

## Independent watchdog (safetyd)
Sees only motiond's heartbeat (cycle counter). Trips if no beat for 60 ms (3 cycles), if the counter does not
advance between beats (wedged loop with live timer), or if no first beat within 500 ms of start.
Latched until reset. Knows nothing about risk or sensors, so a bug in the control path cannot disable it.

## Known limits
- 100 ms CRITICAL confirmation trades reaction time for nuisance-stop resistance; the risk engine already
  commands a crawl speed at CRITICAL, so the vehicle is slowing during that window.
- Not a certified safety function. Thresholds are unvalidated against a real plant.
