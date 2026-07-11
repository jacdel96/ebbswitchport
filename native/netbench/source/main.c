// Standalone link micro-benchmark for the ebbswitchport network-upscale
// setup. Runs as its own NRO so the Switch<->Mac link can be measured in
// isolation — no crypto, no compression, no inference, no game loop.
// Pairs with native/tools/net_bench_server.py (run it on the Mac first).
//
// What it answers, with direct hardware measurements (not theory):
//   1. The per-round-trip latency floor of this link (tiny UDP payloads).
//   2. Whether the Switch's stack can send/receive >MTU UDP datagrams
//      (IP fragmentation) — a 57KB request as ONE datagram is ~40 fragments;
//      if that works, protocol v2 never needs to chunk requests at all.
//   3. The real ingress/egress bandwidth ceiling and loss behavior at line
//      rate — tests the USB2-adapter-bottleneck hypothesis directly.
//   4. TCP vs UDP round trips at the real protocol's exact payload shapes
//      (57KB request -> 180KB response) — quantifies what UDP actually buys.
//
// Server address: read from the existing settings.cfg net_host entry (the
// same address the game's Network Setup screen saved), port fixed at 9877.
// Results go to the screen and to sdmc:/switch/ebbswitchport/netbench.log.

#include <switch.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#define BENCH_PORT 9877
#define SETTINGS_PATH "sdmc:/switch/ebbswitchport/settings.cfg"
#define LOG_PATH "sdmc:/switch/ebbswitchport/netbench.log"
#define TOTAL_RESP 180000  // mirrors net_bench_server.py's response size

// Same reasoning as net_upscale.c: these are up to 64KB, keep them off the
// thread stack (a prior stack-array version of the main app crashed on real
// hardware exactly this way).
static uint8_t s_txbuf[65536];
static uint8_t s_rxbuf[65536];
static uint8_t s_seen[8192];       // ingress dedupe bitmap (seq < 65536)
static uint32_t s_times[512];      // per-test RTT samples for p50
static FILE *s_log;
static uint32_t g_seq = 1;         // global so stale echoes never match a later test

// Bumped UDP buffers vs. socketInitializeDefault (whose UDP defaults are
// 9KB tx / 41KB rx — far too small for multi-fragment datagrams or bursts).
// Also doubles as a hardware validation of this exact config before the
// real protocol v2 relies on it.
static const SocketInitConfig s_sockCfg = {
    .tcp_tx_buf_size     = 0x8000,
    .tcp_rx_buf_size     = 0x10000,
    .tcp_tx_buf_max_size = 0x40000,
    .tcp_rx_buf_max_size = 0x40000,
    .udp_tx_buf_size     = 0x20000,  // 128KB — must hold one 57KB request datagram
    .udp_rx_buf_size     = 0x40000,  // 256KB — absorb a response burst
    .sb_efficiency       = 4,
    .num_bsd_sessions    = 3,
    .bsd_service_type    = BsdServiceType_User,
};

static void logline(const char *fmt, ...) {
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    if (s_log) { fputs(line, s_log); fputc('\n', s_log); fflush(s_log); }
    consoleUpdate(NULL);
}

static u64 now_us(void) { return armTicksToNs(armGetSystemTick()) / 1000ULL; }

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

// Reads the server IP out of the game's saved settings (net_host=ip:port) —
// no input UI needed. Falls back to the known direct-link address.
static void read_server_ip(char *out, size_t cap) {
    snprintf(out, cap, "192.168.2.1");
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[160], host[64];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "net_host=%63[^\n]", host) == 1) {
            char *colon = strrchr(host, ':');
            if (colon) *colon = '\0';
            if (host[0]) snprintf(out, cap, "%s", host);
        }
    }
    fclose(f);
}

// Drains anything sitting in the socket until it's been quiet for `quiet_ms`
// — keeps one test's stragglers (dup BENDs, late chunks) out of the next.
static void drain(int sock, int quiet_ms) {
    for (;;) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, quiet_ms) <= 0) return;
        recv(sock, s_rxbuf, sizeof(s_rxbuf), 0);
    }
}

// One UDP echo round trip (total datagram = 8B header + fill). Returns
// microseconds, or -1 on timeout/error (errno preserved for the caller).
static long udp_ping_once(int sock, size_t total_len, int timeout_ms) {
    uint32_t seq = g_seq++;
    memcpy(s_txbuf, "PING", 4);
    memcpy(s_txbuf + 4, &seq, 4);       // aarch64 is little-endian; matches server's "<I"
    memset(s_txbuf + 8, 0x5a, total_len - 8);
    u64 t0 = now_us();
    if (send(sock, s_txbuf, total_len, 0) != (ssize_t)total_len) return -1;
    u64 deadline = t0 + (u64)timeout_ms * 1000;
    for (;;) {
        u64 now = now_us();
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, (int)((deadline - now) / 1000) + 1) <= 0) { errno = ETIMEDOUT; return -1; }
        ssize_t r = recv(sock, s_rxbuf, sizeof(s_rxbuf), 0);
        if (r < 8 || memcmp(s_rxbuf, "PING", 4) != 0) continue;
        uint32_t rseq;
        memcpy(&rseq, s_rxbuf + 4, 4);
        if (rseq != seq) continue;      // stale echo from an earlier timed-out iteration
        if ((size_t)r != total_len) continue;  // truncated/mangled — treat like a miss
        return (long)(now_us() - t0);
    }
}

static void report_rtt(const char *label, int got, int lost) {
    if (got == 0) {
        logline("%-26s ALL LOST (%d tries)", label, lost);
        return;
    }
    int n = got > 512 ? 512 : got;
    qsort(s_times, n, sizeof(uint32_t), cmp_u32);
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += s_times[i];
    logline("%-26s avg %.2f p50 %.2f min %.2f max %.2f ms, %d/%d lost",
            label, (double)sum / n / 1000.0, s_times[n / 2] / 1000.0,
            s_times[0] / 1000.0, s_times[n - 1] / 1000.0, lost, got + lost);
}

static void run_udp_rtt(int sock, const char *label, size_t total_len, int iters) {
    int got = 0, lost = 0;
    int first_errno = 0;
    for (int i = 0; i < iters; i++) {
        long us = udp_ping_once(sock, total_len, 250);
        if (us < 0) {
            if (!first_errno && errno != ETIMEDOUT) first_errno = errno;
            lost++;
            if (lost == 10 && got == 0) {  // dead test — don't burn 250ms x iters
                logline("%-26s aborted: 10 straight losses%s%s", label,
                        first_errno ? ", errno " : "",
                        first_errno ? strerror(first_errno) : "");
                return;
            }
            continue;
        }
        if (got < 512) s_times[got] = (uint32_t)us;
        got++;
    }
    report_rtt(label, got, lost);
    drain(sock, 100);
}

// Response-shape round trip: tiny trigger out, server replies 180KB as
// `expect_chunks` datagrams; measures trigger-send -> last-chunk-received.
static void run_udp_resp(int sock, const char *label, const char *cmd, int expect_chunks, int iters) {
    int got = 0, lost = 0;
    for (int i = 0; i < iters; i++) {
        uint32_t seq = g_seq++;
        memcpy(s_txbuf, cmd, 4);
        memcpy(s_txbuf + 4, &seq, 4);
        u64 t0 = now_us();
        if (send(sock, s_txbuf, 8, 0) != 8) { lost++; continue; }
        uint64_t have = 0;
        int have_count = 0;
        u64 deadline = t0 + 400000;
        while (have_count < expect_chunks) {
            u64 now = now_us();
            if (now >= deadline) break;
            struct pollfd pfd = { .fd = sock, .events = POLLIN };
            if (poll(&pfd, 1, (int)((deadline - now) / 1000) + 1) <= 0) break;
            ssize_t r = recv(sock, s_rxbuf, sizeof(s_rxbuf), 0);
            if (r < 12 || memcmp(s_rxbuf, "RDAT", 4) != 0) continue;
            uint32_t rseq;
            memcpy(&rseq, s_rxbuf + 4, 4);
            if (rseq != seq) continue;
            uint16_t idx;
            memcpy(&idx, s_rxbuf + 8, 2);
            if (idx >= 64) continue;
            if (!(have & (1ULL << idx))) { have |= 1ULL << idx; have_count++; }
        }
        if (have_count < expect_chunks) { lost++; continue; }
        if (got < 512) s_times[got] = (uint32_t)(now_us() - t0);
        got++;
    }
    report_rtt(label, got, lost);
    drain(sock, 100);
    if (got > 0) {
        // effective one-way throughput of the response burst, from the median RTT
        int n = got > 512 ? 512 : got;
        double mbps = TOTAL_RESP * 8.0 / (double)s_times[n / 2];
        logline("  -> ~%.0f Mbps effective for the 180KB response leg", mbps);
    }
}

// Server -> Switch blast: measures real ingress throughput + loss at a given
// offered rate. `size` = total datagram bytes, pace_us = server-side gap.
static void run_ingress(int sock, const char *label, uint32_t size, uint32_t count, uint32_t pace_us) {
    memset(s_seen, 0, sizeof(s_seen));
    memcpy(s_txbuf, "BLS1", 4);
    memcpy(s_txbuf + 4, &size, 4);
    memcpy(s_txbuf + 8, &count, 4);
    memcpy(s_txbuf + 12, &pace_us, 4);
    if (send(sock, s_txbuf, 16, 0) != 16) {
        logline("%-26s FAILED to request (errno %s)", label, strerror(errno));
        return;
    }
    uint32_t received = 0;
    uint64_t bytes = 0;
    u64 first = 0, last = 0;
    bool ended = false;
    u64 quiet_deadline = now_us() + 2000000;  // 2s to first packet
    for (;;) {
        u64 now = now_us();
        if (now >= quiet_deadline) break;
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, (int)((quiet_deadline - now) / 1000) + 1) <= 0) break;
        ssize_t r = recv(sock, s_rxbuf, sizeof(s_rxbuf), 0);
        if (r < 8) continue;
        if (memcmp(s_rxbuf, "BDAT", 4) == 0) {
            uint32_t seq;
            memcpy(&seq, s_rxbuf + 4, 4);
            if (seq < sizeof(s_seen) * 8 && !(s_seen[seq >> 3] & (1u << (seq & 7)))) {
                s_seen[seq >> 3] |= (uint8_t)(1u << (seq & 7));
                received++;
                bytes += (uint64_t)r;
            }
            u64 t = now_us();
            if (!first) first = t;
            last = t;
            // while data is flowing, only require 700ms of silence to stop
            quiet_deadline = t + 700000;
        } else if (memcmp(s_rxbuf, "BEND", 4) == 0) {
            ended = true;
            quiet_deadline = now_us() + 150000;  // brief drain for stragglers
        }
    }
    double mbps = (last > first && bytes) ? bytes * 8.0 / (double)(last - first) : 0.0;
    logline("%-26s got %u/%u (%.1f%% loss), %.0f Mbps%s",
            label, received, count,
            count ? 100.0 * (count - received) / count : 0.0,
            mbps, ended ? "" : " [no BEND seen]");
    drain(sock, 100);
}

// Switch -> server blast: server counts arrivals and reports back.
static void run_egress(int sock, const char *label, uint32_t size, uint32_t count) {
    memcpy(s_txbuf, "EGRB", 4);
    memcpy(s_txbuf + 4, &count, 4);
    if (send(sock, s_txbuf, 8, 0) != 8) {
        logline("%-26s FAILED to arm (errno %s)", label, strerror(errno));
        return;
    }
    svcSleepThread(50000000ULL);  // 50ms — let the server arm

    memcpy(s_txbuf, "EDAT", 4);
    memset(s_txbuf + 8, 0x6b, size - 8);
    u64 t0 = now_us();
    uint32_t sent = 0;
    for (uint32_t seq = 0; seq < count; seq++) {
        memcpy(s_txbuf + 4, &seq, 4);
        if (send(sock, s_txbuf, size, 0) == (ssize_t)size) sent++;
    }
    u64 t1 = now_us();
    svcSleepThread(100000000ULL);  // let the tail arrive before asking

    uint32_t received = 0, srv_elapsed_us = 0;
    bool got_stats = false;
    for (int tries = 0; tries < 5 && !got_stats; tries++) {
        memcpy(s_txbuf, "EGRQ", 4);
        send(sock, s_txbuf, 4, 0);
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, 300) <= 0) continue;
        ssize_t r = recv(sock, s_rxbuf, sizeof(s_rxbuf), 0);
        if (r >= 12 && memcmp(s_rxbuf, "EGRS", 4) == 0) {
            memcpy(&received, s_rxbuf + 4, 4);
            memcpy(&srv_elapsed_us, s_rxbuf + 8, 4);
            got_stats = true;
        }
    }
    double send_mbps = (t1 > t0) ? (double)sent * size * 8.0 / (double)(t1 - t0) : 0.0;
    if (got_stats) {
        double srv_mbps = srv_elapsed_us ? (double)received * size * 8.0 / (double)srv_elapsed_us : 0.0;
        logline("%-26s sent %u @%.0f Mbps; server got %u/%u (%.1f%% loss) @%.0f Mbps",
                label, sent, send_mbps, received, count,
                count ? 100.0 * (count - received) / count : 0.0, srv_mbps);
    } else {
        logline("%-26s sent %u @%.0f Mbps; NO server stats reply", label, sent, send_mbps);
    }
    drain(sock, 100);
}

// --- TCP helpers (same patterns as the real client) -------------------------

static bool send_all(int sock, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t s = send(sock, p + sent, n - sent, 0);
        if (s <= 0) return false;
        sent += (size_t)s;
    }
    return true;
}

static bool recv_exact(int sock, void *buf, size_t n, int timeout_ms) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        ssize_t r = recv(sock, p + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// Receives and discards n bytes (for replies larger than our buffer).
static bool recv_discard(int sock, size_t n, int timeout_ms) {
    size_t got = 0;
    while (got < n) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        size_t want = n - got;
        if (want > sizeof(s_rxbuf)) want = sizeof(s_rxbuf);
        ssize_t r = recv(sock, s_rxbuf, want, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

static int tcp_connect(const char *host, uint16_t port, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(sock); return -1; }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) { close(sock); return -1; }
    if (rc < 0) {
        struct pollfd pfd = { .fd = sock, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_ms) <= 0) { close(sock); return -1; }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err != 0) { close(sock); return -1; }
    }
    fcntl(sock, F_SETFL, flags);
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return sock;
}

// One TCP round trip: [len u32][mode u8][payload], reply [len u32][payload].
static long tcp_rt_once(int sock, size_t payload_len, uint8_t mode) {
    uint32_t len32 = (uint32_t)payload_len;
    memcpy(s_txbuf, &len32, 4);
    s_txbuf[4] = mode;
    memset(s_txbuf + 5, 0x3c, payload_len > sizeof(s_txbuf) - 5 ? 0 : payload_len);
    u64 t0 = now_us();
    if (!send_all(sock, s_txbuf, 5 + payload_len)) return -1;
    uint32_t rlen;
    if (!recv_exact(sock, &rlen, 4, 1000)) return -1;
    if (rlen > 2 * 1024 * 1024) return -1;
    if (!recv_discard(sock, rlen, 1000)) return -1;
    return (long)(now_us() - t0);
}

static void run_tcp_rtt(int sock, const char *label, size_t payload_len, uint8_t mode, int iters) {
    int got = 0, lost = 0;
    for (int i = 0; i < iters; i++) {
        long us = tcp_rt_once(sock, payload_len, mode);
        if (us < 0) {
            lost++;
            if (lost == 5 && got == 0) {
                logline("%-26s aborted: 5 straight failures", label);
                return;
            }
            continue;
        }
        if (got < 512) s_times[got] = (uint32_t)us;
        got++;
    }
    report_rtt(label, got, lost);
}

int main(int argc, char *argv[]) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/ebbswitchport", 0777);
    s_log = fopen(LOG_PATH, "w");

    logline("=== ebbswitchport link benchmark ===");

    Result rc = socketInitialize(&s_sockCfg);
    if (R_FAILED(rc)) {
        logline("socketInitialize FAILED: 0x%x", rc);
        logline("(this alone is a useful result — the bumped UDP buffer");
        logline(" config doesn't work; protocol v2 must use defaults)");
        goto wait_exit;
    }
    logline("socketInitialize OK (udp tx 128KB / rx 256KB config)");

    char server_ip[64];
    read_server_ip(server_ip, sizeof(server_ip));
    logline("server: %s:%d (from settings.cfg net_host)", server_ip, BENCH_PORT);
    logline("start net_bench_server.py on the Mac if not running");
    logline("");

    // --- UDP socket ---
    int usock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in uaddr;
    memset(&uaddr, 0, sizeof(uaddr));
    uaddr.sin_family = AF_INET;
    uaddr.sin_port = htons(BENCH_PORT);
    if (usock < 0 || inet_pton(AF_INET, server_ip, &uaddr.sin_addr) != 1 ||
        connect(usock, (struct sockaddr *)&uaddr, sizeof(uaddr)) < 0) {
        logline("UDP socket setup FAILED (errno %s)", strerror(errno));
        goto net_exit;
    }
    {
        int want = 0x40000;
        setsockopt(usock, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
        setsockopt(usock, SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
        int rcv = 0, snd = 0;
        socklen_t sl = sizeof(rcv);
        getsockopt(usock, SOL_SOCKET, SO_RCVBUF, &rcv, &sl);
        sl = sizeof(snd);
        getsockopt(usock, SOL_SOCKET, SO_SNDBUF, &snd, &sl);
        logline("UDP buffers granted: rcv %dKB snd %dKB", rcv / 1024, snd / 1024);
    }

    // Reachability precheck so a wrong IP/missing server fails in ~2s, not
    // minutes of per-test timeouts.
    {
        bool reachable = false;
        for (int i = 0; i < 3 && !reachable; i++)
            reachable = udp_ping_once(usock, 32, 500) >= 0;
        if (!reachable) {
            logline("server UNREACHABLE — is net_bench_server.py running?");
            logline("aborting tests");
            close(usock);
            goto net_exit;
        }
    }
    logline("server reachable, running tests...");
    logline("");

    // --- 1. RTT floor + fragmentation viability (sizes are total datagram) ---
    run_udp_rtt(usock, "UDP rtt 32B", 32, 200);
    run_udp_rtt(usock, "UDP rtt 1400B", 1400, 200);
    run_udp_rtt(usock, "UDP rtt 8KB", 8192, 200);
    run_udp_rtt(usock, "UDP rtt 16KB", 16384, 200);
    // THE fragmentation question: request-sized single datagram (~40 fragments)
    run_udp_rtt(usock, "UDP rtt 57KB (req-size)", 57356, 100);
    logline("");

    // --- 2. Response-shaped bursts (180KB, like the compressed response) ---
    run_udp_resp(usock, "UDP resp 180KB=11x16KB", "RSP1", 11, 50);
    run_udp_resp(usock, "UDP resp 180KB=3x60KB", "RSP2", 3, 50);
    logline("");

    // --- 3. Ingress ceiling + loss (Mac -> Switch, ~10MB each) ---
    run_ingress(usock, "UDP in 16KBx640 full", 16384, 640, 0);
    run_ingress(usock, "UDP in 16KBx640 pace250", 16384, 640, 250);
    run_ingress(usock, "UDP in 60KBx170 full", 61440, 170, 0);
    run_ingress(usock, "UDP in 1400Bx3600 full", 1400, 3600, 0);
    logline("");

    // --- 4. Egress ceiling (Switch -> Mac) ---
    run_egress(usock, "UDP out 16KBx640", 16384, 640);
    logline("");
    close(usock);

    // --- 5. TCP comparison, same shapes ---
    {
        int tsock = tcp_connect(server_ip, BENCH_PORT, 3000);
        if (tsock < 0) {
            logline("TCP connect FAILED (errno %s)", strerror(errno));
        } else {
            run_tcp_rtt(tsock, "TCP rtt 32B echo", 32, 0, 200);
            run_tcp_rtt(tsock, "TCP 57KB->180KB shape", 57348, 1, 50);
            close(tsock);
        }
    }

    logline("");
    logline("DONE — read/photograph these numbers.");
    logline("(also written to netbench.log on the SD card)");

net_exit:
    socketExit();
wait_exit:
    logline("");
    logline("press + to exit");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
        svcSleepThread(50000000ULL);
    }
    if (s_log) fclose(s_log);
    consoleExit(NULL);
    return 0;
}
