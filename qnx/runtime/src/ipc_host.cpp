#ifndef __QNX__
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "edgert_ipc.h"

namespace edgert {

static void path_for(const char* name, sockaddr_un* a) {
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    snprintf(a->sun_path, sizeof a->sun_path, "/tmp/edgert_%s.sock", name);
}

// Host stand-in for a QNX pulse: 6 bytes, first byte 0xFF (real message types start 0x01..0x08 at byte 0).
constexpr unsigned char PULSE_MARK = 0xFF;
constexpr int MAXC = 8;

struct Server {
    int lfd;
    int cfd[MAXC];
    int nc;
    bool had_client;
    bool exit_when_idle;
    int8_t pcode;
    uint32_t pval;
    sockaddr_un addr;
};
struct Client { int fd; };
struct Ticker {
    uint64_t start_ns, period_ns, k;
};

Server* server_open(const char* name) {
    Server* s = new Server{};
    s->exit_when_idle = true;
    path_for(name, &s->addr);
    unlink(s->addr.sun_path);
    s->lfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s->lfd < 0 || bind(s->lfd, reinterpret_cast<sockaddr*>(&s->addr), sizeof s->addr) != 0 || listen(s->lfd, 8) != 0) {
        delete s;
        return nullptr;
    }
    return s;
}

int server_receive(Server* s, void* buf, size_t cap, size_t* len, int timeout_ms) {
    const uint64_t deadline = timeout_ms >= 0 ? now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ULL : 0;
    for (;;) {
        int wait = -1;
        if (timeout_ms >= 0) {
            uint64_t now = now_ns();
            if (now >= deadline) return RX_TIMEOUT;
            wait = static_cast<int>((deadline - now + 999999ULL) / 1000000ULL);
        }
        pollfd p[MAXC + 1];
        p[0] = {s->lfd, POLLIN, 0};
        for (int i = 0; i < s->nc; ++i) p[i + 1] = {s->cfd[i], POLLIN, 0};
        int r = poll(p, static_cast<nfds_t>(s->nc + 1), wait);
        if (r < 0) return errno == EINTR ? RX_TIMEOUT : -1;
        if (r == 0) return RX_TIMEOUT;
        if (p[0].revents & POLLIN) {
            int fd = accept(s->lfd, nullptr, nullptr);
            if (fd >= 0 && s->nc < MAXC) { s->cfd[s->nc++] = fd; s->had_client = true; }
            else if (fd >= 0) close(fd);
        }
        for (int i = 0; i < s->nc; ++i) {
            if (!(p[i + 1].revents & (POLLIN | POLLHUP))) continue;
            ssize_t n = recv(s->cfd[i], buf, cap, MSG_TRUNC);  // MSG_TRUNC: report the sender's real size
            if (n <= 0) {
                close(s->cfd[i]);
                s->cfd[i] = s->cfd[--s->nc];
                if (s->nc == 0 && s->had_client && s->exit_when_idle) return RX_STOP;
                break;
            }
            const unsigned char* b = static_cast<const unsigned char*>(buf);
            if (n == 6 && b[0] == PULSE_MARK) {
                s->pcode = static_cast<int8_t>(b[1]);
                memcpy(&s->pval, b + 2, 4);
                return RX_PULSE;
            }
            *len = static_cast<size_t>(n);
            return s->cfd[i];
        }
    }
}

void server_last_pulse(const Server* s, int8_t* code, uint32_t* value) {
    *code = s->pcode;
    *value = s->pval;
}

int server_reply(Server*, int handle, const void* buf, size_t len) {
    return send(handle, buf, len, MSG_NOSIGNAL) == static_cast<ssize_t>(len) ? 0 : -1;
}

void server_set_exit_when_idle(Server* s, bool on) { s->exit_when_idle = on; }

void server_close(Server* s) {
    if (!s) return;
    for (int i = 0; i < s->nc; ++i) close(s->cfd[i]);
    close(s->lfd);
    unlink(s->addr.sun_path);
    delete s;
}

Client* client_open(const char* name) {
    sockaddr_un a;
    path_for(name, &a);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return nullptr;
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { close(fd); return nullptr; }
    return new Client{fd};
}

int client_call(Client* c, const void* req, size_t req_len, void* rep, size_t rep_cap, int timeout_ms) {
    char junk[64];
    while (recv(c->fd, junk, sizeof junk, MSG_DONTWAIT) > 0) {}  // discard late replies to earlier timed-out calls
    if (send(c->fd, req, req_len, MSG_NOSIGNAL) != static_cast<ssize_t>(req_len)) return -1;
    if (timeout_ms >= 0) {
        pollfd p{c->fd, POLLIN, 0};
        int r = poll(&p, 1, timeout_ms);
        if (r == 0) { errno = ETIMEDOUT; return -1; }
        if (r < 0) return -1;
    }
    ssize_t n = recv(c->fd, rep, rep_cap, 0);
    return n < 0 ? -1 : static_cast<int>(n);
}

int client_pulse(Client* c, int8_t code, uint32_t value) {
    unsigned char b[6];
    b[0] = PULSE_MARK;
    b[1] = static_cast<unsigned char>(code);
    memcpy(b + 2, &value, 4);
    return send(c->fd, b, sizeof b, MSG_NOSIGNAL | MSG_DONTWAIT) == static_cast<ssize_t>(sizeof b) ? 0 : -1;
}

void client_close(Client* c) {
    if (!c) return;
    close(c->fd);
    delete c;
}

Ticker* ticker_open(uint32_t period_ms) {
    return new Ticker{now_ns(), static_cast<uint64_t>(period_ms) * 1000000ULL, 0};
}

bool ticker_wait(Ticker* t, uint64_t* scheduled_ns) {
    ++t->k;
    uint64_t at = t->start_ns + t->k * t->period_ns;
    timespec ts;
    ts.tv_sec = static_cast<time_t>(at / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(at % 1000000000ULL);
    int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    if (r == EINTR) return false;
    *scheduled_ns = at;
    return r == 0;
}

void ticker_close(Ticker* t) { delete t; }

int set_fifo_priority(int) { return 0; }  // host build: no RT claims

}  // namespace edgert
#endif
