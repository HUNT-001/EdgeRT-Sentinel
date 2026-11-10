#ifdef __QNX__
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>

#include "edgert_ipc.h"

namespace edgert {

struct Server {
    name_attach_t* att;
    int8_t pcode;
    uint32_t pval;
};
struct Client { int coid; };
struct Ticker {
    int chid, coid;
    timer_t tid;
    uint64_t start_ns, period_ns, k;
};

Server* server_open(const char* name) {
    name_attach_t* a = name_attach(nullptr, name, 0);
    if (!a) return nullptr;
    return new Server{a, 0, 0};
}

int server_receive(Server* s, void* buf, size_t cap, size_t* len, int timeout_ms) {
    for (;;) {
        if (timeout_ms >= 0) {
            uint64_t ns = static_cast<uint64_t>(timeout_ms) * 1000000ULL;
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, nullptr, &ns, nullptr);
        }
        struct _msg_info info;
        int rcvid = MsgReceive(s->att->chid, buf, cap, &info);
        if (rcvid < 0) {
            if (errno == ETIMEDOUT || errno == EINTR) return RX_TIMEOUT;
            return -1;
        }
        if (rcvid == 0) {  // pulse
            const struct _pulse* p = static_cast<const struct _pulse*>(buf);
            if (p->code < 0) continue;  // system pulses (disconnect, coid death): nothing to do
            s->pcode = p->code;
            s->pval = static_cast<uint32_t>(p->value.sival_int);
            return RX_PULSE;
        }
        *len = static_cast<size_t>(info.srcmsglen);
        return rcvid;
    }
}

void server_last_pulse(const Server* s, int8_t* code, uint32_t* value) {
    *code = s->pcode;
    *value = s->pval;
}

int server_reply(Server*, int handle, const void* buf, size_t len) {
    return MsgReply(handle, static_cast<int>(len), buf, len) == -1 ? -1 : 0;  // status = length
}

void server_set_exit_when_idle(Server*, bool) {}

void server_close(Server* s) {
    if (!s) return;
    name_detach(s->att, 0);
    delete s;
}

Client* client_open(const char* name) {
    int coid = name_open(name, 0);
    if (coid < 0) return nullptr;
    return new Client{coid};
}

int client_call(Client* c, const void* req, size_t req_len, void* rep, size_t rep_cap, int timeout_ms) {
    if (timeout_ms >= 0) {
        uint64_t ns = static_cast<uint64_t>(timeout_ms) * 1000000ULL;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, nullptr, &ns, nullptr);
    }
    return MsgSend(c->coid, req, req_len, rep, rep_cap);  // reply status = reply length, or -1
}

int client_pulse(Client* c, int8_t code, uint32_t value) {
    return MsgSendPulse(c->coid, SIGEV_PULSE_PRIO_INHERIT, code, static_cast<int>(value)) == -1 ? -1 : 0;
}

void client_close(Client* c) {
    if (!c) return;
    name_close(c->coid);
    delete c;
}

Ticker* ticker_open(uint32_t period_ms) {
    Ticker* t = new Ticker();  // zero-initialised (timer_t is an integer type on QNX)
    t->period_ns = static_cast<uint64_t>(period_ms) * 1000000ULL;
    t->chid = ChannelCreate(0);
    if (t->chid == -1) { delete t; return nullptr; }
    t->coid = ConnectAttach(0, 0, t->chid, _NTO_SIDE_CHANNEL, 0);
    if (t->coid == -1) { ChannelDestroy(t->chid); delete t; return nullptr; }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, t->coid, SIGEV_PULSE_PRIO_INHERIT, 1, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &t->tid) == -1) {
        ConnectDetach(t->coid); ChannelDestroy(t->chid); delete t; return nullptr;
    }
    itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = static_cast<time_t>(t->period_ns / 1000000000ULL);
    its.it_value.tv_nsec = static_cast<long>(t->period_ns % 1000000000ULL);
    its.it_interval = its.it_value;
    t->start_ns = now_ns();
    if (timer_settime(t->tid, 0, &its, nullptr) == -1) {
        timer_delete(t->tid); ConnectDetach(t->coid); ChannelDestroy(t->chid); delete t; return nullptr;
    }
    return t;
}

bool ticker_wait(Ticker* t, uint64_t* scheduled_ns) {
    struct _pulse p;
    for (;;) {
        if (MsgReceivePulse(t->chid, &p, sizeof p, nullptr) == -1) {
            if (errno == EINTR) return false;  // let the caller check its stop flag
            return false;
        }
        if (p.code == 1) break;
    }
    ++t->k;
    *scheduled_ns = t->start_ns + t->k * t->period_ns;
    return true;
}

void ticker_close(Ticker* t) {
    if (!t) return;
    timer_delete(t->tid);
    ConnectDetach(t->coid);
    ChannelDestroy(t->chid);
    delete t;
}

int set_fifo_priority(int prio) {
    if (prio <= 0) return 0;
    sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

}  // namespace edgert
#endif
