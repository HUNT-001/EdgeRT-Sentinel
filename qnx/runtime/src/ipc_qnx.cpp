#ifdef __QNX__
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "edgert_ipc.h"

namespace edgert {

struct Server { name_attach_t* att; };
struct Client { int coid; };

Server* server_open(const char* name) {
    name_attach_t* a = name_attach(nullptr, name, 0);
    if (!a) return nullptr;
    return new Server{a};
}

int server_receive(Server* s, void* buf, size_t cap, size_t* len) {
    for (;;) {
        struct _msg_info info;
        int rcvid = MsgReceive(s->att->chid, buf, cap, &info);
        if (rcvid < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rcvid == 0) continue;  // pulse (client disconnect etc.): nothing to do
        *len = info.msglen < static_cast<int>(cap) ? static_cast<size_t>(info.msglen) : cap;
        return rcvid;
    }
}

int server_reply(Server*, int handle, const void* buf, size_t len) {
    return MsgReply(handle, static_cast<int>(len), buf, len) == -1 ? -1 : 0;  // status = length
}

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

int client_call(Client* c, const void* req, size_t req_len, void* rep, size_t rep_cap) {
    return MsgSend(c->coid, req, req_len, rep, rep_cap);  // returns the reply status = reply length, or -1
}

void client_close(Client* c) {
    if (!c) return;
    name_close(c->coid);
    delete c;
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
