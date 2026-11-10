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

constexpr int MAXC = 8;
struct Server {
    int lfd;
    int cfd[MAXC];
    int nc;
    bool had_client;
    sockaddr_un addr;
};
struct Client { int fd; };

Server* server_open(const char* name) {
    Server* s = new Server{};
    path_for(name, &s->addr);
    unlink(s->addr.sun_path);
    s->lfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s->lfd < 0 || bind(s->lfd, reinterpret_cast<sockaddr*>(&s->addr), sizeof s->addr) != 0 || listen(s->lfd, 4) != 0) {
        delete s;
        return nullptr;
    }
    return s;
}

int server_receive(Server* s, void* buf, size_t cap, size_t* len) {
    for (;;) {
        pollfd p[MAXC + 1];
        p[0] = {s->lfd, POLLIN, 0};
        for (int i = 0; i < s->nc; ++i) p[i + 1] = {s->cfd[i], POLLIN, 0};
        if (poll(p, static_cast<nfds_t>(s->nc + 1), -1) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (p[0].revents & POLLIN) {
            int fd = accept(s->lfd, nullptr, nullptr);
            if (fd >= 0 && s->nc < MAXC) { s->cfd[s->nc++] = fd; s->had_client = true; }
            else if (fd >= 0) close(fd);
        }
        for (int i = 0; i < s->nc; ++i) {
            if (!(p[i + 1].revents & (POLLIN | POLLHUP))) continue;
            ssize_t n = recv(s->cfd[i], buf, cap, 0);
            if (n <= 0) {  // peer gone
                close(s->cfd[i]);
                s->cfd[i] = s->cfd[--s->nc];
                if (s->nc == 0 && s->had_client) return -2;  // single-session test mode
                break;
            }
            *len = static_cast<size_t>(n);
            return s->cfd[i];
        }
    }
}

int server_reply(Server*, int handle, const void* buf, size_t len) {
    return send(handle, buf, len, MSG_NOSIGNAL) == static_cast<ssize_t>(len) ? 0 : -1;
}

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

int client_call(Client* c, const void* req, size_t req_len, void* rep, size_t rep_cap) {
    if (send(c->fd, req, req_len, MSG_NOSIGNAL) != static_cast<ssize_t>(req_len)) return -1;
    ssize_t n = recv(c->fd, rep, rep_cap, 0);
    return n < 0 ? -1 : static_cast<int>(n);
}

void client_close(Client* c) {
    if (!c) return;
    close(c->fd);
    delete c;
}

int set_fifo_priority(int) { return 0; }  // host build: no RT claims

}  // namespace edgert
#endif
