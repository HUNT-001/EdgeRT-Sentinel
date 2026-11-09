// link_probe — Gate 1 host<->target communication probe.
//
// TCP mode (default): listens on --port, accepts one client, streams --count
// JSON lines at --hz, then exits. This is the transport telemetryd will use.
// Stdout mode (--stdout): writes the same lines to stdout — the serial/log
// fallback when QEMU networking is not ready.
//
// Line format: {"probe":"link","seq":N,"t_ns":T}\n   (T = target CLOCK_MONOTONIC)
//
// Usage: link_probe [--port 5555] [--hz 20] [--count 200] [--stdout]
// Exit:  0 = all lines sent, 1 = client dropped, 2 = setup error

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int64_t kNsPerSec = 1000000000LL;

int64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

bool parse_int(const char* s, int lo, int hi, int& out) {
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

bool send_all(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void sleep_until(int64_t t) {
    timespec ts{static_cast<time_t>(t / kNsPerSec), static_cast<long>(t % kNsPerSec)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

}  // namespace

int main(int argc, char** argv) {
    int port = 5555, hz = 20, count = 200;
    bool to_stdout = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        bool ok = true;
        if (!std::strcmp(a, "--stdout")) { to_stdout = true; continue; }
        if (!std::strcmp(a, "--port") && v)       ok = parse_int(v, 1, 65535, port);
        else if (!std::strcmp(a, "--hz") && v)    ok = parse_int(v, 1, 1000, hz);
        else if (!std::strcmp(a, "--count") && v) ok = parse_int(v, 1, 1000000, count);
        else ok = false;
        if (!ok) {
            std::fprintf(stderr, "usage: %s [--port N] [--hz 1..1000] [--count N] [--stdout]\n", argv[0]);
            return 2;
        }
        ++i;
    }
    std::signal(SIGPIPE, SIG_IGN);

    int cfd = -1;
    if (!to_stdout) {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) { std::perror("socket"); return 2; }
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { std::perror("bind"); return 2; }
        if (listen(lfd, 1) < 0) { std::perror("listen"); return 2; }
        std::fprintf(stderr, "link_probe: listening on 0.0.0.0:%d\n", port);
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        do { cfd = accept(lfd, reinterpret_cast<sockaddr*>(&peer), &plen); } while (cfd < 0 && errno == EINTR);
        close(lfd);
        if (cfd < 0) { std::perror("accept"); return 2; }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        std::fprintf(stderr, "link_probe: client %s connected\n", ip);
    }

    const int64_t period = kNsPerSec / hz;
    const int64_t origin = now_ns();
    char line[128];
    int sent = 0;
    for (int seq = 0; seq < count; ++seq) {
        sleep_until(origin + seq * period);
        int n = std::snprintf(line, sizeof line, "{\"probe\":\"link\",\"seq\":%d,\"t_ns\":%lld}\n", seq,
                              static_cast<long long>(now_ns()));
        if (to_stdout) {
            std::fwrite(line, 1, static_cast<size_t>(n), stdout);
            std::fflush(stdout);
        } else if (!send_all(cfd, line, static_cast<size_t>(n))) {
            std::fprintf(stderr, "link_probe: client dropped after %d lines\n", sent);
            close(cfd);
            return 1;
        }
        ++sent;
    }
    if (cfd >= 0) close(cfd);
    std::fprintf(stderr, "link_probe: sent %d lines\n", sent);
    return 0;
}
