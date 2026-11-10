// Small helpers shared by the services. Header-only on purpose.
#pragma once

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edgert_ipc.h"
#include "edgert_protocol.h"

namespace edgert {

static volatile sig_atomic_t g_stop = 0;

inline void install_stop_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = [](int) { g_stop = 1; };  // no SA_RESTART: blocking calls return EINTR so loops see g_stop
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

inline uint64_t ms_of(uint64_t ns) { return ns / 1000000ULL; }

inline void sleep_ms(unsigned ms) {
    timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

// Services start in any order; clients wait for their peers.
inline Client* connect_retry(const char* name, int total_ms) {
    for (int waited = 0; waited <= total_ms && !g_stop; waited += 50) {
        if (Client* c = client_open(name)) return c;
        sleep_ms(50);
    }
    return nullptr;
}

constexpr const char* NAME_RISKD = "edgert_riskd";
constexpr const char* NAME_MOTIOND = "edgert_motiond";
constexpr const char* NAME_SAFETYD = "edgert_safetyd";
constexpr const char* NAME_ACTUATORD = "edgert_actuatord";

}  // namespace edgert
