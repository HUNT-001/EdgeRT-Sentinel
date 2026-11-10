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
