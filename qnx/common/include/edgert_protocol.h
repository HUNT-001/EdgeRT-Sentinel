// EdgeRT Sentinel IPC protocol, version 1.
//
// Fixed-width, explicitly laid-out messages for QNX MsgSend/MsgReceive and pulses.
// There is no implicit padding: every offset and size is asserted below, so a
// layout change fails the build instead of silently breaking a service.
// Never send these structs over TCP to the host; the host link has its own
// serialisation (JSON Lines), because ABI and endianness need not match.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

#include "risk_types.h"

namespace edgert {

constexpr uint16_t PROTOCOL_VERSION = 1;

// Message types start above 0x1FF so they never collide with QNX io messages
// if a service later becomes a resource manager.
enum class MsgType : uint16_t {
    SENSOR_FRAME = 0x0201,
    RISK_REQUEST = 0x0202,
    RISK_RESPONSE = 0x0203,
    CONTROL_COMMAND = 0x0204,
    SAFETY_COMMAND = 0x0205,
    TELEMETRY_EVENT = 0x0206,
    ACTUATOR_ACK = 0x0207,
    GENERIC_ACK = 0x0208,
    OPERATOR_COMMAND = 0x0209,
    OPERATOR_ACK = 0x020A,
};

// Pulse codes (QNX allows 0..127). The pulse value is a uint32 counter or sequence.
constexpr int8_t PULSE_TICK = 1;       // periodic timer release
constexpr int8_t PULSE_HEARTBEAT = 2;  // motiond -> safetyd, value = control cycle number
constexpr int8_t PULSE_WATCHDOG = 3;   // watchdog expiry notification

enum class SafetyState : uint8_t { NORMAL = 0, DEGRADED = 1, SAFE_STOP = 2 };

enum class SafetyReason : uint8_t {
    NONE = 0,
    SENSOR_STALE = 1,
    RISK_STALE = 2,
    RISK_INVALID = 3,
    DEADLINE_MISS = 4,
    CRITICAL_RISK = 5,
    HEARTBEAT_LOST = 6,
    ACTUATOR_TIMEOUT = 7,
    COMMAND_INVALID = 8,
};

enum class ServiceId : uint16_t { SENSOR = 1, RISK = 2, MOTION = 3, SAFETY = 4, ACTUATOR = 5, TELEMETRY = 6 };

enum class EventCode : uint16_t {
    SERVICE_START = 1,
    CONTROL_CYCLE = 2,
    DEADLINE_MISS = 3,
    SENSOR_FRAME_RX = 4,
    SENSOR_STALE = 5,
    RISK_REQUEST_SENT = 6,
    RISK_RESPONSE_RX = 7,
    RISK_REJECTED = 8,
    STATE_CHANGE = 9,
    COMMAND_ACCEPTED = 10,
    COMMAND_REJECTED = 11,
    WATCHDOG_TRIP = 12,
    FAULT_INJECTED = 13,
    FAULT_CLEARED = 14,
    OPERATOR_RESET = 15,    // value0 = OperatorResult
    ACTUATOR_APPLIED = 16,  // value0 = applied mm/s, value1 = ActuatorSource
};

// Operator commands (opctl -> motiond; motiond -> safetyd). RESET releases a latched SAFE_STOP.
enum class OperatorCmd : uint8_t { RESET = 1 };
enum class OperatorResult : uint8_t {
    OK = 0,             // accepted (for opctl: queued, the outcome is visible as a state change / telemetry)
    NOT_LATCHED = 1,    // nothing to reset
    REJECTED = 2,       // a safety condition still holds
    UNSUPPORTED = 3,   // unknown command
};
enum class ActuatorSource : int32_t { INIT = 0, MOTIOND = 1, SAFETYD = 2, TIMEOUT = 3, RESET = 4 };

constexpr uint8_t SENSOR_FLAG_INVALID = 0x01;
constexpr uint8_t SENSOR_FLAG_REPLAY_END = 0x02;

struct MsgHeader {
    uint16_t type;     // MsgType
    uint16_t version;  // PROTOCOL_VERSION
};

constexpr MsgHeader make_header(MsgType t) { return MsgHeader{static_cast<uint16_t>(t), PROTOCOL_VERSION}; }

// sensor_replayd -> motiond (snapshot), embedded in RiskRequest.
struct SensorFrame {
    uint64_t monotonic_timestamp_ns;
    uint32_t sequence;
    uint32_t obstacle_distance_mm;
    int32_t closing_speed_mm_s;
    uint32_t ego_speed_mm_s;
    uint16_t sensor_age_ms;
    uint8_t confidence_u8;  // 0..255 = 0..100 %
    uint8_t flags;          // SENSOR_FLAG_*
    uint32_t reserved;
};

// sensor_replayd -> motiond. Same layout as RiskRequest on purpose; the type field tells them apart.
struct SensorMsg {
    MsgHeader header;
    uint32_t sequence;
    SensorFrame frame;
};

// Plain acknowledgement (reply to SensorMsg and SafetyCommand).
struct Ack {
    MsgHeader header;
    uint32_t sequence;
};

// actuatord -> motiond, reply to ControlCommand. safety_override=1 means safetyd (or the actuator
// command-timeout) has forced a stop and the commanded speed was NOT applied.
struct ActuatorAck {
    MsgHeader header;
    uint32_t sequence;
    int32_t applied_left_mm_s;
    int32_t applied_right_mm_s;
    uint8_t state;  // SafetyState actuatord is enforcing
    uint8_t reason;
    uint8_t safety_override;
    uint8_t reserved;
};

// motiond risk worker -> riskd.
struct RiskRequest {
    MsgHeader header;
    uint32_t sequence;
    SensorFrame sensor;
};

// riskd -> motiond risk worker. risk_level and status use the values in risk_types.h.
struct RiskResponse {
    MsgHeader header;
    uint32_t sequence;  // echoes RiskRequest.sequence
    uint64_t computed_timestamp_ns;
    uint16_t risk_q15;
    uint16_t risk_level;
    uint32_t time_to_collision_ms;
    uint32_t stopping_distance_mm;
    uint32_t max_safe_speed_mm_s;
    uint16_t status;
    uint16_t reserved0;
    uint32_t reserved1;
};

// motiond publisher -> actuatord.
struct ControlCommand {
    MsgHeader header;
    uint32_t sequence;
    uint64_t monotonic_timestamp_ns;
    int32_t left_velocity_mm_s;
    int32_t right_velocity_mm_s;
    uint8_t state;   // SafetyState the controller believed it was in
    uint8_t reason;  // SafetyReason
    uint8_t reserved[6];
};

// safetyd -> actuatord, independent of motiond.
struct SafetyCommand {
    MsgHeader header;
    uint32_t sequence;
    uint64_t monotonic_timestamp_ns;
    uint8_t state;   // SafetyState to enter
    uint8_t reason;  // SafetyReason
    uint16_t reserved0;
    uint32_t reserved1;
};

// opctl -> motiond, motiond -> safetyd. Reply is an OperatorAck.
struct OperatorCommand {
    MsgHeader header;
    uint32_t sequence;
    uint8_t command;  // OperatorCmd
    uint8_t arg0;
    uint16_t reserved;
};

struct OperatorAck {
    MsgHeader header;
    uint32_t sequence;
    uint8_t result;  // OperatorResult
    uint8_t state;   // SafetyState after handling
    uint8_t reason;  // SafetyReason
    uint8_t reserved;
};

// any service -> telemetryd.
struct TelemetryEvent {
    MsgHeader header;
    uint16_t source;  // ServiceId
    uint16_t event;   // EventCode
    uint64_t monotonic_timestamp_ns;
    uint32_t sequence;
    uint32_t value0;
    int32_t value1;
    uint32_t value2;
};

// ---- layout contract -------------------------------------------------------

#define EDGERT_WIRE_TYPE(T, SIZE)                                                \
    static_assert(sizeof(T) == (SIZE), #T " size changed");                      \
    static_assert(alignof(T) <= 8, #T " alignment above 8");                     \
    static_assert(std::is_standard_layout<T>::value, #T " not standard layout"); \
    static_assert(std::is_trivially_copyable<T>::value, #T " not trivially copyable")

#define EDGERT_OFFSET(T, MEMBER, OFF) static_assert(offsetof(T, MEMBER) == (OFF), #T "::" #MEMBER " offset changed")

EDGERT_WIRE_TYPE(MsgHeader, 4);

EDGERT_WIRE_TYPE(SensorFrame, 32);
EDGERT_OFFSET(SensorFrame, monotonic_timestamp_ns, 0);
EDGERT_OFFSET(SensorFrame, sequence, 8);
EDGERT_OFFSET(SensorFrame, obstacle_distance_mm, 12);
EDGERT_OFFSET(SensorFrame, closing_speed_mm_s, 16);
EDGERT_OFFSET(SensorFrame, ego_speed_mm_s, 20);
EDGERT_OFFSET(SensorFrame, sensor_age_ms, 24);
EDGERT_OFFSET(SensorFrame, confidence_u8, 26);
EDGERT_OFFSET(SensorFrame, flags, 27);
EDGERT_OFFSET(SensorFrame, reserved, 28);

EDGERT_WIRE_TYPE(SensorMsg, 40);
EDGERT_OFFSET(SensorMsg, sequence, 4);
EDGERT_OFFSET(SensorMsg, frame, 8);

EDGERT_WIRE_TYPE(Ack, 8);
EDGERT_OFFSET(Ack, sequence, 4);

EDGERT_WIRE_TYPE(ActuatorAck, 20);
EDGERT_OFFSET(ActuatorAck, sequence, 4);
EDGERT_OFFSET(ActuatorAck, applied_left_mm_s, 8);
EDGERT_OFFSET(ActuatorAck, applied_right_mm_s, 12);
EDGERT_OFFSET(ActuatorAck, state, 16);
EDGERT_OFFSET(ActuatorAck, reason, 17);
EDGERT_OFFSET(ActuatorAck, safety_override, 18);
EDGERT_OFFSET(ActuatorAck, reserved, 19);

EDGERT_WIRE_TYPE(RiskRequest, 40);
EDGERT_OFFSET(RiskRequest, sequence, 4);
EDGERT_OFFSET(RiskRequest, sensor, 8);

EDGERT_WIRE_TYPE(RiskResponse, 40);
EDGERT_OFFSET(RiskResponse, sequence, 4);
EDGERT_OFFSET(RiskResponse, computed_timestamp_ns, 8);
EDGERT_OFFSET(RiskResponse, risk_q15, 16);
EDGERT_OFFSET(RiskResponse, risk_level, 18);
EDGERT_OFFSET(RiskResponse, time_to_collision_ms, 20);
EDGERT_OFFSET(RiskResponse, stopping_distance_mm, 24);
EDGERT_OFFSET(RiskResponse, max_safe_speed_mm_s, 28);
EDGERT_OFFSET(RiskResponse, status, 32);
EDGERT_OFFSET(RiskResponse, reserved0, 34);
EDGERT_OFFSET(RiskResponse, reserved1, 36);

EDGERT_WIRE_TYPE(ControlCommand, 32);
EDGERT_OFFSET(ControlCommand, sequence, 4);
EDGERT_OFFSET(ControlCommand, monotonic_timestamp_ns, 8);
EDGERT_OFFSET(ControlCommand, left_velocity_mm_s, 16);
EDGERT_OFFSET(ControlCommand, right_velocity_mm_s, 20);
EDGERT_OFFSET(ControlCommand, state, 24);
EDGERT_OFFSET(ControlCommand, reason, 25);
EDGERT_OFFSET(ControlCommand, reserved, 26);

EDGERT_WIRE_TYPE(SafetyCommand, 24);
EDGERT_OFFSET(SafetyCommand, sequence, 4);
EDGERT_OFFSET(SafetyCommand, monotonic_timestamp_ns, 8);
EDGERT_OFFSET(SafetyCommand, state, 16);
EDGERT_OFFSET(SafetyCommand, reason, 17);
EDGERT_OFFSET(SafetyCommand, reserved0, 18);
EDGERT_OFFSET(SafetyCommand, reserved1, 20);

EDGERT_WIRE_TYPE(TelemetryEvent, 32);
EDGERT_OFFSET(TelemetryEvent, source, 4);
EDGERT_OFFSET(TelemetryEvent, event, 6);
EDGERT_OFFSET(TelemetryEvent, monotonic_timestamp_ns, 8);
EDGERT_OFFSET(TelemetryEvent, sequence, 16);
EDGERT_OFFSET(TelemetryEvent, value0, 20);
EDGERT_OFFSET(TelemetryEvent, value1, 24);
EDGERT_OFFSET(TelemetryEvent, value2, 28);

EDGERT_WIRE_TYPE(OperatorCommand, 12);
EDGERT_OFFSET(OperatorCommand, sequence, 4);
EDGERT_OFFSET(OperatorCommand, command, 8);
EDGERT_OFFSET(OperatorCommand, arg0, 9);

EDGERT_WIRE_TYPE(OperatorAck, 12);
EDGERT_OFFSET(OperatorAck, sequence, 4);
EDGERT_OFFSET(OperatorAck, result, 8);
EDGERT_OFFSET(OperatorAck, state, 9);
EDGERT_OFFSET(OperatorAck, reason, 10);

}  // namespace edgert
