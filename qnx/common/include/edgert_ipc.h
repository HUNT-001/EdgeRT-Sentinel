// Minimal synchronous request/reply transport used by all services.
// QNX build: name_attach/name_open + MsgReceive/MsgSend/MsgReply (native message passing).
// Host build (HOST=1): Unix SOCK_SEQPACKET, same semantics, for CI and logic tests only.
// Real-time conclusions are drawn only from the QNX build.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace edgert {

inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

struct Server;  // opaque
struct Client;  // opaque
struct Ticker;  // opaque

// Return codes of server_receive besides a reply handle (>= 0).
constexpr int RX_STOP = -2;     // host build only: last client left and exit_when_idle is set
constexpr int RX_PULSE = -3;    // a pulse arrived; read it with server_last_pulse()
constexpr int RX_TIMEOUT = -4;  // timeout elapsed, or a signal interrupted the wait
// buf must be at least 16 bytes (a QNX pulse is written there).

// Create a named service. Returns nullptr on failure (errno set).
Server* server_open(const char* name);
// Block up to timeout_ms (-1 = forever) for the next request or pulse. On a request returns a reply
// handle >= 0 and sets *len to the size the SENDER sent (so oversize messages are detectable).
int server_receive(Server* s, void* buf, size_t cap, size_t* len, int timeout_ms = -1);
void server_last_pulse(const Server* s, int8_t* code, uint32_t* value);
int server_reply(Server* s, int handle, const void* buf, size_t len);
void server_set_exit_when_idle(Server* s, bool on);  // host build only; no-op on QNX (default on)
void server_close(Server* s);

// Connect to a named service. Returns nullptr on failure.
Client* client_open(const char* name);
// Send a request and block up to timeout_ms (-1 = forever) for the reply. Returns reply length, or -1
// on failure (errno ETIMEDOUT on timeout). A late reply to a timed-out call is discarded.
int client_call(Client* c, const void* req, size_t req_len, void* rep, size_t rep_cap, int timeout_ms = -1);
// Fire-and-forget pulse: never blocks the sender. Returns 0 or -1.
int client_pulse(Client* c, int8_t code, uint32_t value);
void client_close(Client* c);

// Periodic release at a fixed period. ticker_wait blocks for the next tick and reports the time that
// tick was SCHEDULED for (CLOCK_MONOTONIC ns), so callers measure real release lateness.
Ticker* ticker_open(uint32_t period_ms);
bool ticker_wait(Ticker* t, uint64_t* scheduled_ns);
void ticker_close(Ticker* t);

// Real-time setup shared by services: SCHED_FIFO at `prio` (0 = leave default). Returns 0 on success.
int set_fifo_priority(int prio);

}  // namespace edgert
