# EdgeRT IPC protocol v1

Header: `qnx/common/include/edgert_protocol.h`. Layout is fixed by `static_assert`s (size, offsets,
standard-layout, trivially copyable); a change that alters any of them fails the build. Bumping
`PROTOCOL_VERSION` is required for any layout change. Receivers reject other versions.

## Transport

| Path | Mechanism | Message |
|---|---|---|
| sensor_replayd -> motiond | MsgSend (sync) | `RiskRequest` carries the `SensorFrame` snapshot |
| motiond risk worker -> riskd | MsgSend, reply = `RiskResponse` | request/response, `sequence` echoed |
| motiond -> actuatord | MsgSend | `ControlCommand` |
| safetyd -> actuatord | MsgSend | `SafetyCommand` (independent path, overrides motiond) |
| any -> telemetryd | MsgSendPulse-style fire and forget via `TelemetryEvent` | never blocks a control path |
| timer -> service | `SIGEV_PULSE` | `PULSE_TICK` |
| motiond -> safetyd | pulse, value = cycle number | `PULSE_HEARTBEAT` |

Messages are native-endian structs between QNX processes only. The host link uses JSON Lines, never these structs.

Additional messages (protocol v1, additive): `SensorMsg` (sensor_replayd -> motiond, 40 B, reply `Ack` 8 B),
`ActuatorAck` (reply to `ControlCommand`, 20 B; `safety_override=1` means the commanded speed was not applied),
`MsgType::ACTUATOR_ACK = 0x0207`, `GENERIC_ACK = 0x0208`.

Every server answers every message it receives (unknown or malformed ones get a fail-safe or generic reply) so no
sender is ever left blocked. Control-path calls (motiond -> riskd, motiond -> actuatord, safetyd -> actuatord) use
a 10 ms timeout; heartbeats are pulses and never block.

## Rules

1. Every message starts with `MsgHeader{type, version}`. Drop and count anything with a wrong type or version.
2. A risk response is usable only if `risk_response_usable()` is true: right type/version, `sequence` equals the outstanding request, `status == RISK_VALID`. Otherwise the controller treats risk as unavailable (degraded/safe-stop per safety policy).
3. `SENSOR_FLAG_INVALID` frames are mapped to an out-of-range distance so the RACE core rejects them (`RISK_INVALID_INPUT`); they are never evaluated.
4. Reserved fields are zeroed by the sender and ignored by the receiver.
5. Time is `CLOCK_MONOTONIC` nanoseconds. Staleness is judged from `sensor_age_ms` and local receive time, never wall clock.
6. No blocking send on a control path without a deadline; telemetry loss is acceptable, control loss is not.

## Sizes

SensorFrame 32, RiskRequest 40, RiskResponse 40, ControlCommand 32, SafetyCommand 24, TelemetryEvent 32 bytes.

## Safety states and reasons

`NORMAL -> DEGRADED -> SAFE_STOP`. Reasons: sensor stale, risk stale/invalid, deadline miss, critical risk, heartbeat lost, actuator timeout, invalid command. Return from SAFE_STOP is an explicit operator action, never automatic.

## Operator reset and telemetry (added after the first runtime)

**OPERATOR_COMMAND (0x0209) / OPERATOR_ACK (0x020A)**, 12 bytes each. `opctl reset` -> motiond -> safetyd.
- motiond answers `NOT_LATCHED` if the controller is not in SAFE_STOP; otherwise it forwards RESET to safetyd.
- safetyd accepts only while heartbeats are fresh (`REJECTED` otherwise), clears its own trip, then sends a
  `SAFETY_COMMAND` with `state=DEGRADED` (meaning "release the latch") to actuatord. motiond alone can never
  unlatch the actuator.
- After `OK`, motiond asks the controller to leave SAFE_STOP for up to 25 cycles (500 ms). The controller still
  requires healthy, fresh, non-critical input, so a reset while the fault persists ends in `MOTION RESET rejected`
  and the system stays stopped. Success logs `MOTION RESET done` and the state goes DEGRADED, then NORMAL after
  25 healthy cycles.
- No authentication: any local process may send these. This is a stated demo assumption.

**Telemetry.** Services emit `TELEMETRY_EVENT` (32 bytes, `value2` added in the old reserved slot) through a
non-blocking ring (`tele_tx.h`): emit() never blocks, a low-priority thread forwards to `telemetryd` with a 5 ms
timed call, a full ring or dead telemetryd drops events. `telemetryd` prints JSON Lines and reports per-source
sequence gaps as `{"ev":"gap","lost":N}`. `control_cycle` (10 Hz): `v0`=speed mm/s, `v1`=distance mm,
`v2`=risk_q15 | level<<16 | state<<20 | reason<<24.
