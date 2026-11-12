// Non-blocking telemetry sender. emit() never blocks and never allocates: it copies one event into a
// fixed ring (try-lock; on contention or a full ring the event is dropped and counted). A low-priority
// thread forwards the ring to telemetryd with a timed call, so a slow or dead telemetryd cannot delay
// a control loop. telemetryd detects drops as gaps in the per-source sequence number.
#pragma once

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "edgert_ipc.h"
#include "edgert_protocol.h"
#include "svc_common.h"

namespace edgert {

constexpr const char* NAME_TELEMETRYD = "edgert_telemetryd";

class TeleTx {
public:
    TeleTx(ServiceId src, int prio) : src_(src), prio_(prio) {
        pthread_mutex_init(&mu_, nullptr);
        started_ = pthread_create(&th_, nullptr, &TeleTx::run, this) == 0;
    }
    ~TeleTx() {
        stop_ = 1;
        if (started_) pthread_join(th_, nullptr);
    }
    void emit(EventCode ev, uint32_t v0 = 0, int32_t v1 = 0, uint32_t v2 = 0) {
        TelemetryEvent e;
        memset(&e, 0, sizeof e);
        e.header = make_header(MsgType::TELEMETRY_EVENT);
        e.source = static_cast<uint16_t>(src_);
        e.event = static_cast<uint16_t>(ev);
        e.monotonic_timestamp_ns = now_ns();
        e.value0 = v0;
        e.value1 = v1;
        e.value2 = v2;
        if (pthread_mutex_trylock(&mu_) != 0) { ++dropped_; return; }
        e.sequence = ++seq_;  // consumed even when dropped, so the receiver sees the gap
        if (count_ == RING) ++dropped_;
        else { ring_[(head_ + count_) % RING] = e; ++count_; }
        pthread_mutex_unlock(&mu_);
    }
    uint64_t dropped() const { return dropped_; }

private:
    static constexpr unsigned RING = 256;
    static void* run(void* p) {
        TeleTx* t = static_cast<TeleTx*>(p);
        set_fifo_priority(t->prio_);
        Client* c = nullptr;
        while (!t->stop_) {
            TelemetryEvent e;
            bool have = false;
            pthread_mutex_lock(&t->mu_);
            if (t->count_) { e = t->ring_[t->head_]; t->head_ = (t->head_ + 1) % RING; --t->count_; have = true; }
            pthread_mutex_unlock(&t->mu_);
            if (!have) { sleep_ms(5); continue; }
            if (!c) c = client_open(NAME_TELEMETRYD);
            if (!c) continue;  // telemetryd not up: the event is lost (and shows as a gap)
            Ack a;
            client_call(c, &e, sizeof e, &a, sizeof a, 5);
        }
        if (c) client_close(c);
        return nullptr;
    }

    ServiceId src_;
    int prio_;
    pthread_mutex_t mu_;
    pthread_t th_;
    bool started_ = false;
    volatile int stop_ = 0;
    TelemetryEvent ring_[RING];
    unsigned head_ = 0, count_ = 0;
    uint32_t seq_ = 0;
    uint64_t dropped_ = 0;
};

}  // namespace edgert
