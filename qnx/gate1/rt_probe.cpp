// rt_probe — Gate 1 feasibility probe for EdgeRT Sentinel.
//
// Proves the QNX 8.0 target can run a periodic real-time loop the way motiond
// will: CLOCK_MONOTONIC timer -> SIGEV_PULSE -> private channel -> SCHED_FIFO
// thread. Reports release jitter (wake time - ideal release) and overruns.
//
// Off-target (HOST=1 build) it falls back to clock_nanosleep(TIMER_ABSTIME) so
// the measurement logic can be exercised on Linux/CI. Host numbers are not
// evidence for the gate; only a QNX run counts.
//
// Usage: rt_probe [--hz 50] [--cycles 500] [--prio 40]
// Exit:  0 = PASS, 1 = FAIL, 2 = setup error

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/utsname.h>

#ifdef __QNX__
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#endif

namespace {

constexpr int64_t kNsPerSec = 1000000000LL;

struct Options {
    int hz = 50;
    int cycles = 500;
    int prio = 40;
};

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

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        bool ok = false;
        if (!std::strcmp(a, "--hz") && v)          ok = parse_int(v, 1, 1000, o.hz);
        else if (!std::strcmp(a, "--cycles") && v) ok = parse_int(v, 10, 1000000, o.cycles);
        else if (!std::strcmp(a, "--prio") && v)   ok = parse_int(v, 1, 255, o.prio);
        if (!ok) {
            std::fprintf(stderr, "usage: %s [--hz 1..1000] [--cycles 10..1000000] [--prio 1..255]\n", argv[0]);
            return false;
        }
        ++i;
    }
    return true;
}

// Returns 0 on success, otherwise errno from pthread_setschedparam.
int set_fifo(int prio) {
    int maxp = sched_get_priority_max(SCHED_FIFO);
    if (maxp > 0 && prio > maxp) prio = maxp;
    sched_param sp{};
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

int64_t percentile(std::vector<int64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
}

#ifdef __QNX__
constexpr int kPulseTick = _PULSE_CODE_MINAVAIL;

struct PulseTimer {
    int chid = -1;
    int coid = -1;
    timer_t tid{};
    bool armed = false;

    bool start(int64_t period_ns) {
        chid = ChannelCreate(0);
        if (chid == -1) { std::perror("ChannelCreate"); return false; }
        coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
        if (coid == -1) { std::perror("ConnectAttach"); return false; }
        sigevent ev{};
        SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, kPulseTick, 0);
        if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) { std::perror("timer_create"); return false; }
        armed = true;
        itimerspec its{};
        its.it_value.tv_sec = period_ns / kNsPerSec;
        its.it_value.tv_nsec = period_ns % kNsPerSec;
        its.it_interval = its.it_value;
        if (timer_settime(tid, 0, &its, nullptr) == -1) { std::perror("timer_settime"); return false; }
        return true;
    }

    // Blocks until the next tick pulse. Returns false on receive error.
    bool wait() {
        for (;;) {
            _pulse p{};
            if (MsgReceivePulse(chid, &p, sizeof p, nullptr) == -1) {
                if (errno == EINTR) continue;
                std::perror("MsgReceivePulse");
                return false;
            }
            if (p.code == kPulseTick) return true;
        }
    }

    ~PulseTimer() {
        if (armed) timer_delete(tid);
        if (coid != -1) ConnectDetach(coid);
        if (chid != -1) ChannelDestroy(chid);
    }
};
#endif

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) return 2;
    const int64_t period = kNsPerSec / o.hz;

    utsname u{};
    uname(&u);
    std::printf("rt_probe: %s %s %s (%s)\n", u.sysname, u.release, u.version, u.machine);

#ifdef __QNX__
    _clockperiod cp{};
    if (ClockPeriod(CLOCK_REALTIME, nullptr, &cp, 0) == 0)
        std::printf("rt_probe: kernel clock period %lu ns\n", static_cast<unsigned long>(cp.nsec));
#else
    std::printf("rt_probe: HOST FALLBACK BUILD (not QNX) - results are not gate evidence\n");
#endif

    int rc = set_fifo(o.prio);
    std::printf("rt_probe: SCHED_FIFO prio %d -> %s\n", o.prio, rc == 0 ? "OK" : std::strerror(rc));
    const bool fifo_ok = (rc == 0);

    std::vector<int64_t> jitter;
    jitter.reserve(static_cast<size_t>(o.cycles));
    int overruns = 0;

#ifdef __QNX__
    PulseTimer timer;
    if (!timer.start(period)) return 2;
    if (!timer.wait()) return 2;  // first tick defines the release origin
    const int64_t origin = now_ns();
    for (int k = 1; k <= o.cycles; ++k) {
        if (!timer.wait()) return 2;
        const int64_t t = now_ns();
        const int64_t j = t - (origin + k * period);
        jitter.push_back(j);
        if (j > period) ++overruns;
    }
#else
    const int64_t origin = now_ns() + period;
    for (int k = 1; k <= o.cycles; ++k) {
        const int64_t rel = origin + k * period;
        timespec ts{static_cast<time_t>(rel / kNsPerSec), static_cast<long>(rel % kNsPerSec)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
        const int64_t j = now_ns() - rel;
        jitter.push_back(j);
        if (j > period) ++overruns;
    }
#endif

    std::vector<int64_t> absj(jitter.size());
    std::transform(jitter.begin(), jitter.end(), absj.begin(), [](int64_t x) { return x < 0 ? -x : x; });
    const int64_t p50 = percentile(absj, 0.50);
    const int64_t p99 = percentile(absj, 0.99);
    const int64_t mx = *std::max_element(absj.begin(), absj.end());

    // Gate 1 criterion: loop runs, priority is settable, no tick is more than
    // half a period late. Tight jitter targets come later, on a fixed QEMU setup.
    const bool pass = fifo_ok && overruns == 0 && mx < period / 2;

    std::printf("rt_probe: hz=%d cycles=%d period_us=%lld\n", o.hz, o.cycles, static_cast<long long>(period / 1000));
    std::printf("rt_probe: |jitter| p50=%lldus p99=%lldus max=%lldus overruns=%d\n",
                static_cast<long long>(p50 / 1000), static_cast<long long>(p99 / 1000),
                static_cast<long long>(mx / 1000), overruns);
    std::printf("{\"probe\":\"rt\",\"hz\":%d,\"cycles\":%d,\"fifo_ok\":%s,\"p50_ns\":%lld,\"p99_ns\":%lld,"
                "\"max_ns\":%lld,\"overruns\":%d,\"pass\":%s}\n",
                o.hz, o.cycles, fifo_ok ? "true" : "false", static_cast<long long>(p50),
                static_cast<long long>(p99), static_cast<long long>(mx), overruns, pass ? "true" : "false");
    std::printf("GATE1 RT PROBE: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
