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

// Create a named service. Returns nullptr on failure (errno set).
Server* server_open(const char* name);
// Block for the next request. Returns a reply handle >= 0 and sets *len, or -1 on fatal error,
// or -2 when the transport asks the server to stop (host build: all clients gone with --once).
// Housekeeping events (QNX disconnect pulses) are consumed internally.
int server_receive(Server* s, void* buf, size_t cap, size_t* len);
int server_reply(Server* s, int handle, const void* buf, size_t len);
void server_close(Server* s);

// Connect to a named service. Returns nullptr on failure.
Client* client_open(const char* name);
// Send a request and block for the reply. Returns reply length, or -1 on failure.
int client_call(Client* c, const void* req, size_t req_len, void* rep, size_t rep_cap);
void client_close(Client* c);

// Real-time setup shared by services: SCHED_FIFO at `prio` (0 = leave default). Returns 0 on success.
int set_fifo_priority(int prio);

}  // namespace edgert
