#include "net_upscale.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include <mbedtls/chachapoly.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#define CHALLENGE_LEN 16
#define HMAC_LEN 32
#define NONCE_LEN 12
#define TAG_LEN 16
#define MAX_HOST_LEN 64
#define MAX_CODE_LEN 64
#define MAX_W 512
#define MAX_H 512
#define MAX_OUT_W (MAX_W * 3)
#define MAX_OUT_H (MAX_H * 3)

// Bounded well under any real frame (256x224 in, 768x672 out); catches a
// malformed/malicious response before it's used to size a memcpy.
#define MAX_PLAINTEXT (MAX_OUT_W * MAX_OUT_H + 4)

static Thread g_thread;
static Mutex g_lock;
static bool g_run = false;      // background thread's run flag
static bool g_ready = false;    // net_upscale_init succeeded, thread is alive
static bool g_connected = false; // handshake completed, frames flowing

static char g_host[MAX_HOST_LEN];
static uint16_t g_port;
static char g_pairing_code[MAX_CODE_LEN];

// Pending outbound frame (mutex-guarded). g_send_pending marks unconsumed data.
static uint8_t g_send_luma[MAX_W * MAX_H];
static unsigned g_send_w = 0, g_send_h = 0;
static bool g_send_pending = false;

// Latest inbound result (mutex-guarded). Double-buffered isn't needed since
// the caller copies out (or reads) while holding no lock across frames —
// net_upscale_get_result documents "valid until the next call".
static uint8_t g_result_luma[MAX_OUT_W * MAX_OUT_H];
static unsigned g_result_w = 0, g_result_h = 0;
static bool g_have_result = false;

static uint64_t g_send_nonce_ctr;
static uint64_t g_recv_nonce_ctr;
static mbedtls_chachapoly_context g_aead;
static bool g_aead_keyed = false;

static void nonce_from_counter(uint64_t ctr, unsigned char out[NONCE_LEN]) {
    memset(out, 0, NONCE_LEN);
    for (int i = 0; i < 8; i++) out[i] = (unsigned char)(ctr >> (8 * i));
}

// HKDF(SHA-256, salt, ikm, info) -> 32 bytes. Mirrors
// net_upscale_server.py's derive_psk_key/derive_session_key exactly —
// both sides must agree on every argument or the handshake silently fails
// (each side derives a different key, decryption fails, connection drops).
static int hkdf32(const unsigned char *salt, size_t salt_len,
                   const unsigned char *ikm, size_t ikm_len,
                   const unsigned char *info, size_t info_len,
                   unsigned char out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_hkdf(md, salt, salt_len, ikm, ikm_len, info, info_len, out, 32);
}

static bool recv_exact(int sock, void *buf, size_t n, int timeout_ms) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return false;  // timeout or error
        ssize_t r = recv(sock, p + got, n - got, 0);
        if (r <= 0) return false;  // closed or error
        got += (size_t)r;
    }
    return true;
}

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

// Sends one AEAD-sealed frame: [u32 len][nonce(12)][ciphertext][tag(16)],
// where len covers everything after itself. Matches
// net_upscale_server.py's SecureChannel.send wire format exactly.
static bool secure_send(int sock, const uint8_t *plaintext, size_t len) {
    unsigned char nonce[NONCE_LEN];
    nonce_from_counter(g_send_nonce_ctr++, nonce);

    uint8_t *ct = malloc(len);
    if (!ct) return false;
    unsigned char tag[TAG_LEN];
    int rc = mbedtls_chachapoly_encrypt_and_tag(&g_aead, len, nonce, NULL, 0, plaintext, ct, tag);
    if (rc != 0) { free(ct); return false; }

    uint32_t frame_len = (uint32_t)(NONCE_LEN + len + TAG_LEN);
    bool ok = send_all(sock, &frame_len, 4) &&
              send_all(sock, nonce, NONCE_LEN) &&
              send_all(sock, ct, len) &&
              send_all(sock, tag, TAG_LEN);
    free(ct);
    return ok;
}

// Receives and opens one AEAD-sealed frame into `out` (caller-sized buffer,
// at least MAX_PLAINTEXT). Returns the plaintext length, or -1 on any
// failure (timeout, malformed length, decrypt/auth failure, oversized
// frame) — all treated identically by the caller: drop the connection and
// let the retry loop reconnect, never trust a partially-validated frame.
static int secure_recv(int sock, uint8_t *out, int timeout_ms) {
    uint32_t frame_len;
    if (!recv_exact(sock, &frame_len, 4, timeout_ms)) return -1;
    if (frame_len < NONCE_LEN + TAG_LEN || frame_len > NONCE_LEN + MAX_PLAINTEXT + TAG_LEN)
        return -1;

    size_t body_len = frame_len - NONCE_LEN;
    uint8_t *body = malloc(body_len);
    if (!body) return -1;
    if (!recv_exact(sock, body, body_len, timeout_ms)) { free(body); return -1; }

    unsigned char nonce[NONCE_LEN];
    nonce_from_counter(g_recv_nonce_ctr, nonce);
    size_t ct_len = body_len - TAG_LEN;
    const unsigned char *ct = body;
    const unsigned char *tag = body + ct_len;

    int rc = mbedtls_chachapoly_auth_decrypt(&g_aead, ct_len, nonce, NULL, 0, tag, ct, out);
    free(body);
    if (rc != 0) return -1;  // wrong nonce sequence or tampered/corrupt data
    g_recv_nonce_ctr++;
    return (int)ct_len;
}

// Connects, runs the challenge/response handshake, and derives the
// per-session AEAD key. Returns false on any failure (unreachable host,
// timeout, wrong pairing code) — the caller just retries later.
static bool do_handshake(int sock) {
    unsigned char challenge[CHALLENGE_LEN];
    if (!recv_exact(sock, challenge, CHALLENGE_LEN, 5000)) return false;

    unsigned char psk_key[32];
    if (hkdf32((const unsigned char *)"ebbswitchport-espcn-psk", 23,
               (const unsigned char *)g_pairing_code, strlen(g_pairing_code),
               (const unsigned char *)"psk", 3, psk_key) != 0) return false;

    unsigned char response[HMAC_LEN];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, psk_key, sizeof(psk_key), challenge, CHALLENGE_LEN, response) != 0)
        return false;
    if (!send_all(sock, response, HMAC_LEN)) return false;

    unsigned char session_key[32];
    if (hkdf32(challenge, CHALLENGE_LEN, psk_key, sizeof(psk_key),
               (const unsigned char *)"ebbswitchport-espcn-session", 27, session_key) != 0)
        return false;

    mbedtls_chachapoly_free(&g_aead);
    mbedtls_chachapoly_init(&g_aead);
    if (mbedtls_chachapoly_setkey(&g_aead, session_key) != 0) return false;
    g_aead_keyed = true;
    g_send_nonce_ctr = 0;
    g_recv_nonce_ctr = 0;
    return true;
}

// Non-blocking connect with a bounded timeout — a plain blocking connect()
// to an unreachable host can hang for the OS's full TCP timeout (tens of
// seconds), which would stall this thread's retry loop for far too long.
static int connect_with_timeout(const char *host, uint16_t port, int timeout_ms) {
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
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) { close(sock); return -1; }
        int err = 0; socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err != 0) { close(sock); return -1; }
    }
    fcntl(sock, F_SETFL, flags);  // back to blocking for the rest of the session
    return sock;
}

static void net_thread_func(void *arg) {
    (void)arg;
    while (g_run) {
        int sock = connect_with_timeout(g_host, g_port, 3000);
        if (sock < 0) { svcSleepThread(1000000000ULL); continue; }  // 1s before retry

        if (!do_handshake(sock)) {
            close(sock);
            svcSleepThread(1000000000ULL);
            continue;
        }

        mutexLock(&g_lock);
        g_connected = true;
        mutexUnlock(&g_lock);

        uint8_t recv_buf[MAX_PLAINTEXT];
        while (g_run) {
            mutexLock(&g_lock);
            bool have_send = g_send_pending;
            unsigned sw = g_send_w, sh = g_send_h;
            uint8_t send_copy[MAX_W * MAX_H];
            if (have_send) memcpy(send_copy, g_send_luma, (size_t)sw * sh);
            g_send_pending = false;
            mutexUnlock(&g_lock);

            if (!have_send) { svcSleepThread(1000000ULL); continue; }  // 1ms idle poll

            uint8_t payload[4 + MAX_W * MAX_H];
            payload[0] = (uint8_t)(sw & 0xFF); payload[1] = (uint8_t)(sw >> 8);
            payload[2] = (uint8_t)(sh & 0xFF); payload[3] = (uint8_t)(sh >> 8);
            memcpy(payload + 4, send_copy, (size_t)sw * sh);

            if (!secure_send(sock, payload, 4 + (size_t)sw * sh)) break;

            int n = secure_recv(sock, recv_buf, 500);  // 500ms: generous vs the
                                                        // ~7-20ms measured on a
                                                        // real laptop+LAN, still
                                                        // far under a stalled frame
            if (n < 4) break;
            unsigned ow = recv_buf[0] | (recv_buf[1] << 8);
            unsigned oh = recv_buf[2] | (recv_buf[3] << 8);
            if (ow > MAX_OUT_W || oh > MAX_OUT_H || (size_t)(n - 4) != (size_t)ow * oh) break;

            mutexLock(&g_lock);
            memcpy(g_result_luma, recv_buf + 4, (size_t)ow * oh);
            g_result_w = ow; g_result_h = oh;
            g_have_result = true;
            mutexUnlock(&g_lock);
        }

        mutexLock(&g_lock);
        g_connected = false;
        mutexUnlock(&g_lock);
        close(sock);
        if (g_run) svcSleepThread(1000000000ULL);  // 1s before reconnect attempt
    }
}

bool net_upscale_init(const char *host_port, const char *pairing_code) {
    const char *colon = strrchr(host_port, ':');
    if (!colon || colon == host_port) return false;
    size_t host_len = (size_t)(colon - host_port);
    if (host_len >= MAX_HOST_LEN) return false;
    if (strlen(pairing_code) >= MAX_CODE_LEN) return false;

    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return false;

    memcpy(g_host, host_port, host_len);
    g_host[host_len] = '\0';
    g_port = (uint16_t)port;
    strncpy(g_pairing_code, pairing_code, MAX_CODE_LEN - 1);
    g_pairing_code[MAX_CODE_LEN - 1] = '\0';

    if (R_FAILED(socketInitializeDefault())) return false;

    mutexInit(&g_lock);
    g_connected = false;
    g_have_result = false;
    g_send_pending = false;
    g_aead_keyed = false;
    g_run = true;

    if (R_FAILED(threadCreate(&g_thread, net_thread_func, NULL, NULL, 0x4000, 0x2C, 1)) ||
        R_FAILED(threadStart(&g_thread))) {
        g_run = false;
        socketExit();
        return false;
    }
    g_ready = true;
    return true;
}

void net_upscale_submit_frame(const uint8_t *luma, unsigned w, unsigned h) {
    if (!g_ready || w == 0 || h == 0 || w > MAX_W || h > MAX_H) return;
    mutexLock(&g_lock);
    memcpy(g_send_luma, luma, (size_t)w * h);
    g_send_w = w; g_send_h = h;
    g_send_pending = true;
    mutexUnlock(&g_lock);
}

bool net_upscale_get_result(const uint8_t **out_luma, unsigned *out_w, unsigned *out_h) {
    if (!g_ready) return false;
    mutexLock(&g_lock);
    bool have = g_have_result;
    if (have) {
        *out_luma = g_result_luma;
        *out_w = g_result_w;
        *out_h = g_result_h;
    }
    mutexUnlock(&g_lock);
    return have;
}

bool net_upscale_connected(void) {
    if (!g_ready) return false;
    mutexLock(&g_lock);
    bool c = g_connected;
    mutexUnlock(&g_lock);
    return c;
}

void net_upscale_exit(void) {
    if (!g_ready) return;
    g_ready = false;
    g_run = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    if (g_aead_keyed) { mbedtls_chachapoly_free(&g_aead); g_aead_keyed = false; }
    socketExit();
}
