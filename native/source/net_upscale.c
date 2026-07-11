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

#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <zstd.h>

// ---- protocol v2 (UDP) — must stay in lockstep with net_upscale_server.py ----
// See net_upscale.h for the full protocol description and security model.
static const uint8_t NET_MAGIC[4] = { 'E', 'B', 'U', '2' };
#define T_HELLO      0x01
#define T_CHALLENGE  0x02
#define T_AUTH       0x03
#define T_ACCEPT     0x04
#define T_REJECT     0x05
#define T_PING       0x06
#define T_FRAME_REQ  0x10
#define T_FRAME_RESP 0x11

#define FLAG_COMP     1  // payload is zstd-compressed
#define FLAG_DIFF     2  // payload (after decompression) is XOR vs frame ref_id
#define FLAG_NEED_KEY 4  // requests only: reply to this frame with a keyframe
#define FLAG_NACK     8  // responses only: request diff undecodable server-side
#define FLAG_MC       16 // responses only: the diff was taken against the
                          // reference SHIFTED by (mc_dx, mc_dy) output-space
                          // pixels, zero-filled at the revealed edges — motion
                          // compensation for camera scroll, which otherwise
                          // makes XOR diffs dense exactly when frames are most
                          // frequent. The server estimates the shift; this
                          // side only has to rebuild the shifted reference.
#define MC_MAX_SHIFT  96 // output-space bound (3x the server's 32px input bound)
#define MC_MAX_INPUT_SHIFT 32 // input-space bound for request-side MC — the
                               // header's mc fields carry INPUT-space pixels
                               // on requests and OUTPUT-space on responses

#define FRAME_HDR_LEN 50
#define REQ_CHUNK 61440   // request chunk payload — a real 256x224 frame (57KB)
                           // fits in ONE datagram (bench-verified: ~40 IP
                           // fragments, 0/100 lost on the direct link)
#define MAX_CHUNKS 256

#define CHALLENGE_LEN 16
#define HMAC_LEN 32
#define MAX_HOST_LEN 64
#define MAX_CODE_LEN 64
#define MAX_W 512
#define MAX_H 512
#define MAX_OUT_W (MAX_W * 3)
#define MAX_OUT_H (MAX_H * 3)
#define RESP_MAX (MAX_OUT_W * MAX_OUT_H)

// Writer thread: opens the socket, handshakes, then sends requests and spawns
// the reader thread. Reader thread: receives/reassembles responses. Splitting
// send and receive across two threads is what lets more than one request be
// in flight (pipelining) — a single thread would serialize on each response.
static Thread g_thread;         // writer
static Thread g_reader_thread;
static Mutex g_lock;
static bool g_run = false;      // background thread's run flag
static bool g_ready = false;    // net_upscale_init succeeded, thread is alive
static bool g_connected = false; // handshake completed, frames flowing
static unsigned g_last_rtt_us = 0; // wall-clock time of the last full
                                    // send+recv round trip, for the HUD

static char g_host[MAX_HOST_LEN];
static uint16_t g_port;
static char g_pairing_code[MAX_CODE_LEN];
static bool g_want_compression;  // the user's setting (init /
                                  // net_upscale_set_compression); sent in the
                                  // AUTH datagram each handshake
static bool g_session_compression;  // what THIS session actually negotiated —
                                     // snapshotted at handshake and used for
                                     // the request-diff path, so a mid-session
                                     // toggle can never half-apply (requests
                                     // diffing while responses stay raw — a
                                     // real mixed state hit on hardware).
                                     // "off" means raw full frames both
                                     // directions, the clean A/B baseline.

// Pending outbound frame (g_lock-guarded). g_send_pending marks unconsumed data.
static uint8_t g_send_luma[MAX_W * MAX_H];
static unsigned g_send_w = 0, g_send_h = 0;
static bool g_send_pending = false;
static uint64_t g_pending_hash = 0;  // hash of g_send_luma, carried alongside it so
                                     // the network thread can cache the eventual
                                     // result under the same key it was requested with

// In-flight request tracking (g_inflight_lock — separate from g_lock, which
// guards the submit/result state the game thread touches; this one is the
// writer/reader coordination point). Matched by frame_id, not FIFO order:
// UDP gives no ordering guarantee, and a lost response must only cost that
// one frame. Depth stays capped at 2 — bounded staleness, no bufferbloat.
#define MAX_INFLIGHT 2
typedef struct {
    bool used;
    uint32_t frame_id;
    uint64_t hash;
    unsigned w, h;
    uint64_t t0;  // armGetSystemTick() at send, for this request's own RTT
} InflightEntry;
static Mutex g_inflight_lock;
static InflightEntry g_inflight[MAX_INFLIGHT];
// Session/diff-sync state, also under g_inflight_lock (small cluster shared
// between exactly the writer and reader threads):
static bool g_conn_alive = false;      // either thread flips false on failure
static uint64_t g_token;               // session token from ACCEPT
static unsigned g_loss_count;          // consecutive expired-unanswered frames
static uint32_t g_acked_input_id;      // newest input the server confirmed
                                        // holding (responses' peer_id) — the
                                        // only frame request diffs may reference
static uint32_t g_held_output_id;      // newest output we decoded — declared
                                        // in every request (requests' peer_id)
                                        // so response diffs only ever reference
                                        // something we provably have
static bool g_need_out_key;            // set on response-diff ref miss; asks
                                        // the server for a keyframe via the
                                        // next request's NEED_KEY flag
static int g_last_resp_shift_dx;       // input-space scroll the server last
static int g_last_resp_shift_dy;       // reported (response mc / 3) — a prime
                                        // candidate for request-side motion
                                        // compensation, since scroll velocity
                                        // is near-constant frame to frame

static uint32_t g_next_frame_id;       // writer-only
static int g_last_req_shift_dx, g_last_req_shift_dy;  // writer-only: last
                                        // shift that actually won — continuity
                                        // candidate for the next frame
// Directional-input hint from the game thread (net_upscale_hint_input):
// which way the player is pushing directly predicts scroll direction, and
// unlike the last-response shift it's current the instant a walk starts,
// stops, or turns. Plain ints, advisory only — no lock needed.
static volatile int g_hint_dir_x, g_hint_dir_y;

// Small result cache, keyed by content hash — catches frames that are
// pixel-identical to a recent one even when the SNES core doesn't flag them
// as a dupe (that flag only means "core skipped rendering entirely"; a
// static scene, paused animation, or idle loop still renders "new" frames
// that are byte-for-byte the same). A cache hit skips the network call
// entirely, not just the model compute.
#define CACHE_SLOTS 3
#define CACHE_TTL_NS (5ULL * 1000000000ULL)  // 5 seconds
typedef struct {
    bool valid;
    uint64_t hash;
    unsigned in_w, in_h;    // input (request) dims — the lookup key, alongside hash
    unsigned out_w, out_h;  // whatever the server actually reported for this input;
                             // not assumed to be a fixed multiple of in_w/in_h
    uint64_t timestamp_ns;  // armTicksToNs(armGetSystemTick()) at insertion
    uint8_t luma[MAX_OUT_W * MAX_OUT_H];
} CacheEntry;
static CacheEntry g_cache[CACHE_SLOTS];
static int g_cache_next_slot = 0;  // round-robin eviction — simplest option that
                                    // doesn't need per-entry access tracking

// FNV-1a, 64-bit — fast, non-cryptographic, plenty for "is this the same
// frame we already have a result for" (not a security boundary; the actual
// wire protocol's authentication is the AEAD... now HMAC handshake in
// do_handshake).
static uint64_t fnv1a(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Must be called with g_lock held. Returns the matching slot index, or -1.
static int cache_find(uint64_t hash, unsigned in_w, unsigned in_h) {
    uint64_t now_ns = armTicksToNs(armGetSystemTick());
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!g_cache[i].valid) continue;
        if (g_cache[i].hash != hash || g_cache[i].in_w != in_w || g_cache[i].in_h != in_h) continue;
        if (now_ns - g_cache[i].timestamp_ns > CACHE_TTL_NS) continue;
        return i;
    }
    return -1;
}

// Must be called with g_lock held.
static void cache_insert(uint64_t hash, unsigned in_w, unsigned in_h,
                          unsigned out_w, unsigned out_h, const uint8_t *luma) {
    int slot = g_cache_next_slot;
    g_cache_next_slot = (g_cache_next_slot + 1) % CACHE_SLOTS;
    g_cache[slot].valid = true;
    g_cache[slot].hash = hash;
    g_cache[slot].in_w = in_w; g_cache[slot].in_h = in_h;
    g_cache[slot].out_w = out_w; g_cache[slot].out_h = out_h;
    g_cache[slot].timestamp_ns = armTicksToNs(armGetSystemTick());
    memcpy(g_cache[slot].luma, luma, (size_t)out_w * out_h);
}

// Latest inbound result (g_lock-guarded).
static uint8_t g_result_luma[MAX_OUT_W * MAX_OUT_H];
static unsigned g_result_w = 0, g_result_h = 0;
static bool g_have_result = false;
static unsigned g_result_generation = 0;  // bumped every time a NEW result lands —
                                           // net_upscale_get_result() re-returns the
                                           // same latched result on every poll, so
                                           // callers that need "is this actually new"
                                           // (e.g. HUD timing) compare this counter.
static uint64_t g_last_result_hash = 0;   // request hash the latest result answers —
                                           // net_upscale_submit_and_wait matches on
                                           // this (not just the generation counter)
                                           // so a stale straggler can't be mistaken
                                           // for the current frame's answer.

// All large scratch MUST be static, not stack locals: the network threads'
// stacks are 32KB and several of these are megabytes. A prior version had
// buffers as stack arrays and crashed the instant the thread started (a
// real bug hit while first testing this on hardware).
//
// Writer-thread-only:
static uint8_t s_send_copy[MAX_W * MAX_H];
static uint8_t s_diff[MAX_W * MAX_H];                       // request XOR scratch
static uint8_t s_shift_scratch[MAX_W * MAX_H];              // shifted-reference build
static uint8_t s_comp[ZSTD_COMPRESSBOUND(MAX_W * MAX_H)];   // request zstd output
static uint8_t s_dgram[FRAME_HDR_LEN + REQ_CHUNK];          // outgoing datagram build
// Input ring: the last few frames we SENT, so request diffs can reference
// whichever one the server has ACKed holding (g_acked_input_id).
#define IN_RING 4
static uint8_t s_in_ring[IN_RING][MAX_W * MAX_H];
static uint32_t s_in_ring_id[IN_RING];
static unsigned s_in_ring_w[IN_RING], s_in_ring_h[IN_RING];
static unsigned s_in_ring_next;
//
// Reader-thread-only:
static uint8_t s_rdgram[65536];                             // one incoming datagram
static uint8_t s_decompressed[RESP_MAX];                    // zstd output scratch
// Response reassembly: 2 slots — with pipelining depth 2, two responses can
// be in flight, though the single-threaded server responder rarely interleaves
// their chunks in practice.
#define REASM_SLOTS 2
typedef struct {
    bool used;
    uint32_t frame_id;
    uint8_t flags;
    unsigned out_w, out_h;
    uint32_t total_len, uncomp_len, ref_id, peer_id;
    int mc_dx, mc_dy;
    unsigned chunk_count, received;
    uint8_t have[MAX_CHUNKS / 8];
} Reasm;
static Reasm s_reasm[REASM_SLOTS];
static uint8_t s_reasm_buf[REASM_SLOTS][RESP_MAX];
// Output ring: the last few DECODED outputs, so response diffs can reference
// whichever one we declared holding when the request went out. 3 slots
// covers pipelining depth 2 (a reference can be up to 2 frames behind).
#define OUT_RING 3
static uint8_t s_out_ring[OUT_RING][RESP_MAX];
static uint32_t s_out_ring_id[OUT_RING];
static unsigned s_out_ring_w[OUT_RING], s_out_ring_h[OUT_RING];

// ---- little helpers ---------------------------------------------------------

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

typedef struct {
    uint8_t type, flags;
    unsigned chunk_idx, chunk_count;
    unsigned w, h;
    uint32_t frame_id, total_len, uncomp_len, offset, ref_id, peer_id;
    int mc_dx, mc_dy;
    uint64_t token;
} FrameHdr;

static void frame_hdr_write(uint8_t *p, const FrameHdr *fh) {
    memcpy(p, NET_MAGIC, 4);
    p[4] = fh->type;
    p[5] = fh->flags;
    put_u16(p + 6, (uint16_t)fh->chunk_idx);
    put_u16(p + 8, (uint16_t)fh->chunk_count);
    put_u16(p + 10, (uint16_t)fh->w);
    put_u16(p + 12, (uint16_t)fh->h);
    put_u32(p + 14, fh->frame_id);
    put_u32(p + 18, fh->total_len);
    put_u32(p + 22, fh->uncomp_len);
    put_u32(p + 26, fh->offset);
    put_u32(p + 30, fh->ref_id);
    put_u32(p + 34, fh->peer_id);
    put_u16(p + 38, (uint16_t)(int16_t)fh->mc_dx);
    put_u16(p + 40, (uint16_t)(int16_t)fh->mc_dy);
    put_u64(p + 42, fh->token);
}

static bool frame_hdr_read(const uint8_t *p, int n, FrameHdr *fh) {
    if (n < FRAME_HDR_LEN || memcmp(p, NET_MAGIC, 4) != 0) return false;
    fh->type = p[4];
    fh->flags = p[5];
    fh->chunk_idx = get_u16(p + 6);
    fh->chunk_count = get_u16(p + 8);
    fh->w = get_u16(p + 10);
    fh->h = get_u16(p + 12);
    fh->frame_id = get_u32(p + 14);
    fh->total_len = get_u32(p + 18);
    fh->uncomp_len = get_u32(p + 22);
    fh->offset = get_u32(p + 26);
    fh->ref_id = get_u32(p + 30);
    fh->peer_id = get_u32(p + 34);
    fh->mc_dx = (int16_t)get_u16(p + 38);
    fh->mc_dy = (int16_t)get_u16(p + 40);
    fh->token = get_u64(p + 42);
    return true;
}

// Waits up to timeout_ms for one datagram. Returns its length, 0 on timeout,
// -1 on socket error. (UDP: each recv() delivers one whole datagram.)
static int recv_dgram(int sock, uint8_t *buf, size_t cap, int timeout_ms) {
    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0) return 0;
    if (pr < 0) return -1;
    ssize_t r = recv(sock, buf, cap, 0);
    return (r <= 0) ? -1 : (int)r;
}

// send() with a short retry loop — the link bench showed the Switch's UDP
// send() fails outright (rather than blocking) when the tx buffer is
// momentarily full; everything that did send arrived with zero wire loss.
static bool send_dgram_retry(int sock, const uint8_t *buf, size_t len) {
    for (int tries = 0; tries < 50; tries++) {
        if (send(sock, buf, len, 0) == (ssize_t)len) return true;
        svcSleepThread(200000ULL);  // 200us — tx drains at wire speed
    }
    return false;
}

// XOR of two equal-size buffers into dst, u64-wide with a byte tail —
// a byte-wise loop over 516KB costs real milliseconds on this CPU.
// dst may alias a (in-place XOR): each index is read before it's written.
static void xor_buffers(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t n) {
    size_t words = n / 8;
    uint64_t *d64 = (uint64_t *)dst;
    const uint64_t *a64 = (const uint64_t *)a;
    const uint64_t *b64 = (const uint64_t *)b;
    for (size_t i = 0; i < words; i++) d64[i] = a64[i] ^ b64[i];
    for (size_t i = words * 8; i < n; i++) dst[i] = a[i] ^ b[i];
}

// dst = ref shifted by (dx, dy) with zero fill at the revealed edges —
// shifted(y, x) = ref(y - dy, x - dx). Row-wise memcpy/memset, so it costs
// about one 516KB copy (~0.2ms), same as the unshifted path's memcpy.
static void build_shifted(uint8_t *dst, const uint8_t *ref,
                           unsigned ow, unsigned oh, int dx, int dy) {
    unsigned copy_w = (unsigned)((int)ow - (dx < 0 ? -dx : dx));
    unsigned dst_x = (unsigned)(dx > 0 ? dx : 0);
    unsigned src_x = (unsigned)(dx < 0 ? -dx : 0);
    for (int y = 0; y < (int)oh; y++) {
        uint8_t *drow = dst + (size_t)y * ow;
        int sy = y - dy;
        if (sy < 0 || sy >= (int)oh) {
            memset(drow, 0, ow);
            continue;
        }
        const uint8_t *srow = ref + (size_t)sy * ow;
        if (dst_x) memset(drow, 0, dst_x);
        memcpy(drow + dst_x, srow + src_x, copy_w);
        if (dst_x + copy_w < ow) memset(drow + dst_x + copy_w, 0, ow - dst_x - copy_w);
    }
}

// Sampled count of mismatching pixels between the current frame and the
// reference shifted by (dx, dy) — the referee that picks which candidate
// shift (if any) a request diff should use. No prediction has to be RIGHT;
// wrong candidates just lose this comparison. Margins exceed
// MC_MAX_INPUT_SHIFT so shifted lookups never leave the buffer; stride-7
// sampling is ~500 points on a real frame, a few microseconds each call.
static unsigned eval_shift_sampled(const uint8_t *cur, const uint8_t *ref,
                                    unsigned w, unsigned h, int dx, int dy) {
    unsigned mismatch = 0;
    for (unsigned y = 40; y + 40 < h; y += 7) {
        const uint8_t *crow = cur + (size_t)y * w;
        const uint8_t *rrow = ref + (size_t)((int)y - dy) * w;
        for (unsigned x = 40; x + 40 < w; x += 7)
            if (crow[x] != rrow[(int)x - dx]) mismatch++;
    }
    return mismatch;
}

// HKDF(SHA-256, salt, ikm, info) -> 32 bytes. Mirrors
// net_upscale_server.py's derive_psk_key exactly — both sides must agree on
// every argument or the handshake fails (different keys, HMAC mismatch).
static int hkdf32(const unsigned char *salt, size_t salt_len,
                   const unsigned char *ikm, size_t ikm_len,
                   const unsigned char *info, size_t info_len,
                   unsigned char out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_hkdf(md, salt, salt_len, ikm, ikm_len, info, info_len, out, 32);
}

// ---- session lifecycle ------------------------------------------------------

static int udp_open(const char *host, uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(sock); return -1; }
    // UDP connect() never blocks — it just pins the peer so plain send/recv
    // work and the kernel filters datagrams from other sources.
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return -1; }
    int buf = 0x40000;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    return sock;
}

// UDP handshake with per-step retries (datagrams can drop, though the bench
// measured zero loss at this traffic shape on the direct link). On success
// sets g_token. The pairing code never crosses the wire — HMAC over a fresh
// random challenge, so a capture can't be replayed.
static bool do_handshake(int sock) {
    unsigned char psk_key[32];
    if (hkdf32((const unsigned char *)"ebbswitchport-espcn-psk", 23,
               (const unsigned char *)g_pairing_code, strlen(g_pairing_code),
               (const unsigned char *)"psk", 3, psk_key) != 0) return false;

    unsigned char challenge[CHALLENGE_LEN];
    bool got_challenge = false;
    for (int attempt = 0; attempt < 3 && !got_challenge; attempt++) {
        uint8_t hello[5];
        memcpy(hello, NET_MAGIC, 4);
        hello[4] = T_HELLO;
        if (send(sock, hello, sizeof(hello), 0) != (ssize_t)sizeof(hello)) return false;
        for (int strays = 0; strays < 8; strays++) {
            int n = recv_dgram(sock, s_rdgram, sizeof(s_rdgram), 300);
            if (n <= 0) break;  // timeout/error -> next attempt
            if (n == 5 + CHALLENGE_LEN && memcmp(s_rdgram, NET_MAGIC, 4) == 0 &&
                s_rdgram[4] == T_CHALLENGE) {
                memcpy(challenge, s_rdgram + 5, CHALLENGE_LEN);
                got_challenge = true;
                break;
            }
        }
    }
    if (!got_challenge) return false;

    unsigned char mac[HMAC_LEN];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, psk_key, sizeof(psk_key), challenge, CHALLENGE_LEN, mac) != 0)
        return false;

    uint8_t auth[5 + HMAC_LEN + 1];
    memcpy(auth, NET_MAGIC, 4);
    auth[4] = T_AUTH;
    memcpy(auth + 5, mac, HMAC_LEN);
    bool session_compression = g_want_compression;  // snapshot: this is what the
                                                     // session negotiates, whatever
                                                     // the toggle does later
    auth[5 + HMAC_LEN] = session_compression ? 1 : 0;
    g_session_compression = session_compression;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (send(sock, auth, sizeof(auth), 0) != (ssize_t)sizeof(auth)) return false;
        for (int strays = 0; strays < 8; strays++) {
            int n = recv_dgram(sock, s_rdgram, sizeof(s_rdgram), 300);
            if (n <= 0) break;
            if (n < 5 || memcmp(s_rdgram, NET_MAGIC, 4) != 0) continue;
            if (s_rdgram[4] == T_ACCEPT && n >= 5 + 8 + 1) {
                g_token = get_u64(s_rdgram + 5);
                return true;
            }
            if (s_rdgram[4] == T_REJECT) return false;  // wrong pairing code
        }
    }
    return false;
}

// ---- reader half of the pipeline --------------------------------------------
// Owns s_rdgram, s_reasm*, s_decompressed, s_out_ring exclusively. Runs for
// one connection's lifetime (spawned by the writer after handshake, joined
// during teardown).

static void reader_deliver(const InflightEntry *entry, const uint8_t *out_luma,
                            unsigned ow, unsigned oh) {
    g_last_rtt_us = (unsigned)(armTicksToNs(armGetSystemTick() - entry->t0) / 1000);
    mutexLock(&g_lock);
    memcpy(g_result_luma, out_luma, (size_t)ow * oh);
    g_result_w = ow;
    g_result_h = oh;
    g_have_result = true;
    g_result_generation++;
    g_last_result_hash = entry->hash;
    cache_insert(entry->hash, entry->w, entry->h, ow, oh, out_luma);
    mutexUnlock(&g_lock);
}

static void reader_thread_func(void *arg) {
    int sock = (int)(intptr_t)arg;
    memset(s_reasm, 0, sizeof(s_reasm));
    memset(s_out_ring_id, 0, sizeof(s_out_ring_id));

    while (g_run) {
        mutexLock(&g_inflight_lock);
        bool alive = g_conn_alive;
        mutexUnlock(&g_inflight_lock);
        if (!alive) return;

        int n = recv_dgram(sock, s_rdgram, sizeof(s_rdgram), 100);
        if (n < 0) {
            mutexLock(&g_inflight_lock);
            g_conn_alive = false;
            mutexUnlock(&g_inflight_lock);
            return;
        }
        if (n == 0) continue;  // timeout: loss is handled by the writer's slot expiry

        if (n >= 5 && memcmp(s_rdgram, NET_MAGIC, 4) == 0 && s_rdgram[4] == T_REJECT) {
            // Server no longer knows this session (restart, idle GC) —
            // tear down and let the writer re-handshake.
            mutexLock(&g_inflight_lock);
            g_conn_alive = false;
            mutexUnlock(&g_inflight_lock);
            return;
        }

        FrameHdr fh;
        if (!frame_hdr_read(s_rdgram, n, &fh) || fh.type != T_FRAME_RESP || fh.token != g_token)
            continue;

        if (fh.flags & FLAG_NACK) {
            // Server couldn't decode our request diff (it lost the reference
            // input) — reset the ACK so the next requests go out raw, and
            // free the in-flight slot for this frame.
            mutexLock(&g_inflight_lock);
            g_acked_input_id = 0;
            for (int i = 0; i < MAX_INFLIGHT; i++)
                if (g_inflight[i].used && g_inflight[i].frame_id == fh.frame_id)
                    g_inflight[i].used = false;
            mutexUnlock(&g_inflight_lock);
            continue;
        }

        size_t part_len = (size_t)n - FRAME_HDR_LEN;
        // Reject anything inconsistent before it can size a memcpy.
        if (fh.w == 0 || fh.h == 0 || fh.w > MAX_OUT_W || fh.h > MAX_OUT_H) continue;
        if (fh.uncomp_len != (uint32_t)fh.w * fh.h || fh.uncomp_len > RESP_MAX) continue;
        if (fh.total_len == 0 || fh.total_len > RESP_MAX) continue;
        if (!(fh.flags & FLAG_COMP) && fh.total_len != fh.uncomp_len) continue;
        if ((fh.flags & FLAG_DIFF) && (!(fh.flags & FLAG_COMP) || fh.ref_id == 0)) continue;
        if ((fh.flags & FLAG_MC) && !(fh.flags & FLAG_DIFF)) continue;
        if (fh.mc_dx < -MC_MAX_SHIFT || fh.mc_dx > MC_MAX_SHIFT ||
            fh.mc_dy < -MC_MAX_SHIFT || fh.mc_dy > MC_MAX_SHIFT) continue;
        if (fh.chunk_count == 0 || fh.chunk_count > MAX_CHUNKS || fh.chunk_idx >= fh.chunk_count) continue;
        if (fh.offset > fh.total_len || part_len > fh.total_len - fh.offset) continue;

        // Find or claim a reassembly slot for this frame_id.
        Reasm *rs = NULL;
        for (int i = 0; i < REASM_SLOTS; i++)
            if (s_reasm[i].used && s_reasm[i].frame_id == fh.frame_id) { rs = &s_reasm[i]; break; }
        if (!rs) {
            for (int i = 0; i < REASM_SLOTS; i++)
                if (!s_reasm[i].used) { rs = &s_reasm[i]; break; }
            if (!rs)  // both busy — evict the older partial (ids are monotonic)
                rs = (s_reasm[0].frame_id < s_reasm[1].frame_id) ? &s_reasm[0] : &s_reasm[1];
            memset(rs, 0, sizeof(*rs));
            rs->used = true;
            rs->frame_id = fh.frame_id;
            rs->flags = fh.flags;
            rs->out_w = fh.w;
            rs->out_h = fh.h;
            rs->total_len = fh.total_len;
            rs->uncomp_len = fh.uncomp_len;
            rs->ref_id = fh.ref_id;
            rs->peer_id = fh.peer_id;
            rs->mc_dx = fh.mc_dx;
            rs->mc_dy = fh.mc_dy;
            rs->chunk_count = fh.chunk_count;
        } else if (rs->total_len != fh.total_len || rs->chunk_count != fh.chunk_count ||
                   rs->out_w != fh.w || rs->out_h != fh.h || rs->flags != fh.flags) {
            rs->used = false;  // inconsistent chunks for one id — drop the frame
            continue;
        }
        int slot = (int)(rs - s_reasm);
        unsigned byte_idx = fh.chunk_idx >> 3, bit = 1u << (fh.chunk_idx & 7);
        if (!(rs->have[byte_idx] & bit)) {
            memcpy(s_reasm_buf[slot] + fh.offset, s_rdgram + FRAME_HDR_LEN, part_len);
            rs->have[byte_idx] |= (uint8_t)bit;
            rs->received++;
        }
        if (rs->received < rs->chunk_count) continue;

        // ---- complete response ----
        rs->used = false;
        unsigned ow = rs->out_w, oh = rs->out_h;
        size_t out_len = (size_t)ow * oh;
        const uint8_t *plane;
        if (rs->flags & FLAG_COMP) {
            size_t d = ZSTD_decompress(s_decompressed, sizeof(s_decompressed),
                                       s_reasm_buf[slot], rs->total_len);
            if (ZSTD_isError(d) || d != rs->uncomp_len) continue;
            plane = s_decompressed;
        } else {
            plane = s_reasm_buf[slot];
        }

        // Pick the destination output-ring slot: never the one a diff reads
        // from, otherwise the oldest (smallest id).
        int ref_slot = -1;
        if (rs->flags & FLAG_DIFF) {
            for (int i = 0; i < OUT_RING; i++)
                if (s_out_ring_id[i] == rs->ref_id &&
                    s_out_ring_w[i] == ow && s_out_ring_h[i] == oh) { ref_slot = i; break; }
            if (ref_slot < 0) {
                // We no longer have (or never had) the reference — can't
                // decode. Ask for a keyframe via the next request and drop
                // this frame; the game keeps showing the last good result.
                mutexLock(&g_inflight_lock);
                g_need_out_key = true;
                for (int i = 0; i < MAX_INFLIGHT; i++)
                    if (g_inflight[i].used && g_inflight[i].frame_id == rs->frame_id)
                        g_inflight[i].used = false;
                mutexUnlock(&g_inflight_lock);
                continue;
            }
        }
        int dst = -1;
        for (int i = 0; i < OUT_RING; i++) {
            if (i == ref_slot) continue;
            if (dst < 0 || s_out_ring_id[i] < s_out_ring_id[dst]) dst = i;
        }
        if ((rs->flags & FLAG_MC) && (rs->mc_dx || rs->mc_dy)) {
            // Motion-compensated diff: rebuild the shifted reference the
            // server diffed against, then XOR the diff plane in place.
            build_shifted(s_out_ring[dst], s_out_ring[ref_slot], ow, oh,
                          rs->mc_dx, rs->mc_dy);
            xor_buffers(s_out_ring[dst], s_out_ring[dst], plane, out_len);
        } else if (rs->flags & FLAG_DIFF) {
            xor_buffers(s_out_ring[dst], s_out_ring[ref_slot], plane, out_len);
        } else {
            memcpy(s_out_ring[dst], plane, out_len);
        }
        s_out_ring_id[dst] = rs->frame_id;
        s_out_ring_w[dst] = ow;
        s_out_ring_h[dst] = oh;

        // Sync state + in-flight match.
        InflightEntry entry;
        bool found = false;
        mutexLock(&g_inflight_lock);
        g_held_output_id = rs->frame_id;
        if (rs->peer_id > g_acked_input_id) g_acked_input_id = rs->peer_id;
        if (rs->flags & FLAG_MC) {
            // Remember the server's scroll estimate (input-space) — the
            // writer uses it as a candidate for request-side motion
            // compensation. Kept when a non-MC frame arrives (menus pause
            // scroll but walks resume at the same velocity); it's only ever
            // a candidate, so staleness costs nothing.
            g_last_resp_shift_dx = rs->mc_dx / 3;
            g_last_resp_shift_dy = rs->mc_dy / 3;
        }
        for (int i = 0; i < MAX_INFLIGHT; i++)
            if (g_inflight[i].used && g_inflight[i].frame_id == rs->frame_id) {
                entry = g_inflight[i];
                g_inflight[i].used = false;
                found = true;
                break;
            }
        if (found) g_loss_count = 0;
        mutexUnlock(&g_inflight_lock);

        if (found)
            reader_deliver(&entry, s_out_ring[dst], ow, oh);
        // else: a straggler whose slot already expired — the decoded frame
        // still updated the ring/held id (it's valid state), just not shown.
    }
}

// ---- writer half of the pipeline ---------------------------------------------
// Owns the socket lifecycle, handshake, s_send_copy/s_diff/s_comp/s_dgram,
// and the input ring.

// Builds and sends one request. Returns false on a dead socket.
static bool send_request(int sock, unsigned sw, unsigned sh, uint32_t fid) {
    uint32_t total_raw = (uint32_t)sw * sh;
    const uint8_t *payload = s_send_copy;
    uint32_t payload_len = total_raw;
    uint8_t flags = 0;
    uint32_t ref_id = 0;

    mutexLock(&g_inflight_lock);
    uint32_t acked = g_acked_input_id;
    uint32_t held = g_held_output_id;
    bool need_key = g_need_out_key;
    g_need_out_key = false;  // sending the flag once is enough; if that
                              // request is lost, the ref-miss recurs and
                              // re-sets it — self-healing
    mutexUnlock(&g_inflight_lock);

    int mc_dx = 0, mc_dy = 0;
    if (g_session_compression && acked != 0) {
        int ref = -1;
        for (int i = 0; i < IN_RING; i++)
            if (s_in_ring_id[i] == acked && s_in_ring_w[i] == sw && s_in_ring_h[i] == sh) { ref = i; break; }
        if (ref >= 0) {
            // Motion compensation for the request leg: camera scroll makes
            // the plain XOR dense right when frames are most frequent, so
            // requests were falling back to raw 57KB exactly during motion.
            // The Switch can't afford real motion ESTIMATION — but it can
            // afford to TEST a few candidate shifts with the sampled referee
            // and use whichever explains the frame best. None of the
            // predictions has to be right: a wrong shift just loses the
            // comparison (or, worst case, produces a dense diff that the
            // existing raw fallback catches). Candidates:
            //   - (0, 0): the plain diff — menus/dialogue, scroll stopped
            //   - the server's last reported shift (and 2x it, since the
            //     ACKed reference can lag two frames): nails steady scroll
            //   - the last shift that won here: continuity
            //   - controller-direction guesses at a couple of magnitudes,
            //     both sign conventions: current the instant a walk starts,
            //     stops, or turns, when the last-shift candidates lag
            if (sw > 80 && sh > 80) {
                int hx = g_hint_dir_x, hy = g_hint_dir_y;
                mutexLock(&g_inflight_lock);
                int lrx = g_last_resp_shift_dx, lry = g_last_resp_shift_dy;
                mutexUnlock(&g_inflight_lock);
                int cand[20][2];
                int ncand = 0;
                cand[ncand][0] = 0; cand[ncand][1] = 0; ncand++;
                // Wider net than the first cut: hardware logs showed whole
                // scroll stretches falling back to raw because the actual
                // per-network-frame shift (walk speed x however many game
                // frames elapsed) landed between the sparse magnitudes
                // {2,4}. The referee is microseconds per candidate — a
                // denser set costs nothing and catches the in-between and
                // drifting-velocity cases: the last-response shift's +-1
                // neighborhood, and hint magnitudes 1..6.
                int raw_cand[][2] = {
                    { lrx, lry }, { 2 * lrx, 2 * lry },
                    { lrx + 1, lry }, { lrx - 1, lry },
                    { lrx, lry + 1 }, { lrx, lry - 1 },
                    { g_last_req_shift_dx, g_last_req_shift_dy },
                    { -1 * hx, -1 * hy }, { -2 * hx, -2 * hy },
                    { -3 * hx, -3 * hy }, { -4 * hx, -4 * hy },
                    { -6 * hx, -6 * hy }, { 2 * hx, 2 * hy },
                };
                for (unsigned c = 0; c < sizeof(raw_cand) / sizeof(raw_cand[0]); c++) {
                    int dx = raw_cand[c][0], dy = raw_cand[c][1];
                    if (dx == 0 && dy == 0) continue;
                    if (dx < -MC_MAX_INPUT_SHIFT || dx > MC_MAX_INPUT_SHIFT ||
                        dy < -MC_MAX_INPUT_SHIFT || dy > MC_MAX_INPUT_SHIFT) continue;
                    bool dup = false;
                    for (int k = 0; k < ncand; k++)
                        if (cand[k][0] == dx && cand[k][1] == dy) { dup = true; break; }
                    if (!dup && ncand < 20) { cand[ncand][0] = dx; cand[ncand][1] = dy; ncand++; }
                }
                unsigned zero_score = eval_shift_sampled(s_send_copy, s_in_ring[ref], sw, sh, 0, 0);
                unsigned best_score = zero_score;
                for (int c = 1; c < ncand; c++) {
                    unsigned e = eval_shift_sampled(s_send_copy, s_in_ring[ref], sw, sh,
                                                    cand[c][0], cand[c][1]);
                    if (e < best_score) { best_score = e; mc_dx = cand[c][0]; mc_dy = cand[c][1]; }
                }
                // Only shift when it's CLEARLY better — a marginal win isn't
                // worth the extra shifted-copy work or a noisier diff.
                if ((mc_dx || mc_dy) && !(best_score * 4 <= zero_score * 3)) {
                    mc_dx = mc_dy = 0;
                }
            }

            const uint8_t *diff_ref = s_in_ring[ref];
            if (mc_dx || mc_dy) {
                build_shifted(s_shift_scratch, s_in_ring[ref], sw, sh, mc_dx, mc_dy);
                diff_ref = s_shift_scratch;
            }
            xor_buffers(s_diff, s_send_copy, diff_ref, total_raw);
            // Sparsity pre-check before spending CPU on zstd: sample every
            // 64th byte; a scene cut (or a scroll none of the candidates
            // explained) produces a dense XOR plane that compresses poorly —
            // cheaper to just send raw. (This is also, incidentally, a free
            // scene-change detector.)
            unsigned nonzero = 0, samples = 0;
            for (uint32_t i = 0; i < total_raw; i += 64, samples++)
                if (s_diff[i]) nonzero++;
            if (samples > 0 && nonzero * 4 < samples) {  // < 25% dense
                size_t c = ZSTD_compress(s_comp, sizeof(s_comp), s_diff, total_raw, 1);
                if (!ZSTD_isError(c) && c < total_raw) {
                    payload = s_comp;
                    payload_len = (uint32_t)c;
                    flags = FLAG_COMP | FLAG_DIFF;
                    if (mc_dx || mc_dy) flags |= FLAG_MC;
                    ref_id = acked;
                }
            }
        }
    }
    if (!(flags & FLAG_DIFF)) mc_dx = mc_dy = 0;  // raw fallback carries no shift
    g_last_req_shift_dx = mc_dx;
    g_last_req_shift_dy = mc_dy;
    if (need_key) flags |= FLAG_NEED_KEY;

    unsigned count = (payload_len + REQ_CHUNK - 1) / REQ_CHUNK;
    if (count == 0) count = 1;
    FrameHdr fh = {
        .type = T_FRAME_REQ, .flags = flags, .chunk_count = count,
        .w = sw, .h = sh, .frame_id = fid, .total_len = payload_len,
        .uncomp_len = total_raw, .ref_id = ref_id, .peer_id = held,
        .mc_dx = mc_dx, .mc_dy = mc_dy,
        .token = g_token,
    };
    for (unsigned i = 0; i < count; i++) {
        uint32_t off = i * REQ_CHUNK;
        uint32_t part = payload_len - off;
        if (part > REQ_CHUNK) part = REQ_CHUNK;
        fh.chunk_idx = i;
        fh.offset = off;
        frame_hdr_write(s_dgram, &fh);
        memcpy(s_dgram + FRAME_HDR_LEN, payload + off, part);
        if (!send_dgram_retry(sock, s_dgram, FRAME_HDR_LEN + part)) return false;
    }

    // Input ring: remember what we sent under this id — a future request may
    // diff against it once the server ACKs holding it.
    s_in_ring_id[s_in_ring_next] = fid;
    s_in_ring_w[s_in_ring_next] = sw;
    s_in_ring_h[s_in_ring_next] = sh;
    memcpy(s_in_ring[s_in_ring_next], s_send_copy, total_raw);
    s_in_ring_next = (s_in_ring_next + 1) % IN_RING;
    return true;
}

static void net_thread_func(void *arg) {
    (void)arg;
    while (g_run) {
        int sock = udp_open(g_host, g_port);
        if (sock < 0) { svcSleepThread(1000000000ULL); continue; }  // 1s before retry

        if (!do_handshake(sock)) {
            close(sock);
            svcSleepThread(1000000000ULL);
            continue;
        }

        // Fresh session: all diff/sync state resets — both sides start from
        // keyframes until references get established and ACKed again.
        mutexLock(&g_inflight_lock);
        memset(g_inflight, 0, sizeof(g_inflight));
        g_loss_count = 0;
        g_acked_input_id = 0;
        g_held_output_id = 0;
        g_need_out_key = false;
        g_conn_alive = true;
        mutexUnlock(&g_inflight_lock);
        memset(s_in_ring_id, 0, sizeof(s_in_ring_id));
        s_in_ring_next = 0;

        mutexLock(&g_lock);
        g_connected = true;
        mutexUnlock(&g_lock);

        if (R_FAILED(threadCreate(&g_reader_thread, reader_thread_func,
                                   (void *)(intptr_t)sock, NULL, 0x8000, 0x2C, 1)) ||
            R_FAILED(threadStart(&g_reader_thread))) {
            mutexLock(&g_inflight_lock);
            g_conn_alive = false;
            mutexUnlock(&g_inflight_lock);
            close(sock);
            mutexLock(&g_lock);
            g_connected = false;
            mutexUnlock(&g_lock);
            svcSleepThread(1000000000ULL);
            continue;
        }

        u64 last_tx_ns = armTicksToNs(armGetSystemTick());
        while (g_run) {
            mutexLock(&g_inflight_lock);
            bool alive = g_conn_alive;
            mutexUnlock(&g_inflight_lock);
            if (!alive) break;

            mutexLock(&g_lock);
            bool have_send = g_send_pending;
            unsigned sw = g_send_w, sh = g_send_h;
            uint64_t pending_hash = g_pending_hash;
            if (have_send) memcpy(s_send_copy, g_send_luma, (size_t)sw * sh);
            g_send_pending = false;
            mutexUnlock(&g_lock);

            u64 now_ns = armTicksToNs(armGetSystemTick());
            if (!have_send) {
                if (now_ns - last_tx_ns > 20ULL * 1000000000ULL) {
                    // Idle keep-alive so the server's 60s session GC doesn't
                    // reap us during a long pause/menu stretch.
                    uint8_t ping[13];
                    memcpy(ping, NET_MAGIC, 4);
                    ping[4] = T_PING;
                    put_u64(ping + 5, g_token);
                    send(sock, ping, sizeof(ping), 0);
                    last_tx_ns = now_ns;
                }
                svcSleepThread(1000000ULL);  // 1ms idle poll
                continue;
            }

            // Reclaim expired in-flight slots (response never came) — each
            // one counts toward the consecutive-loss session-death check.
            int slot = -1;
            bool too_many_losses = false;
            mutexLock(&g_inflight_lock);
            for (int i = 0; i < MAX_INFLIGHT; i++) {
                if (g_inflight[i].used &&
                    armTicksToNs(armGetSystemTick() - g_inflight[i].t0) > 250ULL * 1000000ULL) {
                    g_inflight[i].used = false;
                    g_loss_count++;
                }
                if (!g_inflight[i].used && slot < 0) slot = i;
            }
            if (g_loss_count >= 8) { g_conn_alive = false; too_many_losses = true; }
            mutexUnlock(&g_inflight_lock);
            if (too_many_losses) break;
            if (slot < 0) {
                // Both slots in flight — drop this frame rather than queue
                // it (newest-wins policy: net_upscale_submit_frame will hand
                // us something fresher as soon as a slot frees up).
                svcSleepThread(1000000ULL);
                continue;
            }

            uint32_t fid = ++g_next_frame_id;
            u64 t0 = armGetSystemTick();

            // Register before sending: on this link a response can't beat the
            // send, but there's no reason to leave even a theoretical window.
            mutexLock(&g_inflight_lock);
            g_inflight[slot] = (InflightEntry){ .used = true, .frame_id = fid,
                                                 .hash = pending_hash, .w = sw, .h = sh, .t0 = t0 };
            mutexUnlock(&g_inflight_lock);

            if (!send_request(sock, sw, sh, fid)) {
                mutexLock(&g_inflight_lock);
                g_inflight[slot].used = false;
                g_conn_alive = false;
                mutexUnlock(&g_inflight_lock);
                break;
            }
            last_tx_ns = armTicksToNs(armGetSystemTick());
        }

        threadWaitForExit(&g_reader_thread);
        threadClose(&g_reader_thread);

        mutexLock(&g_lock);
        g_connected = false;
        mutexUnlock(&g_lock);
        close(sock);
        if (g_run) svcSleepThread(1000000000ULL);  // 1s before reconnect attempt
    }
}

// ---- public API ---------------------------------------------------------------

static const SocketInitConfig s_sockCfg = {
    // socketInitializeDefault's UDP buffers (9KB tx / 41KB rx) are far too
    // small for this protocol: requests go out as single ~57KB datagrams and
    // responses arrive as multi-chunk bursts. These values were validated on
    // real hardware by native/netbench (0% loss at the real traffic shape).
    .tcp_tx_buf_size     = 0x8000,
    .tcp_rx_buf_size     = 0x10000,
    .tcp_tx_buf_max_size = 0x40000,
    .tcp_rx_buf_max_size = 0x40000,
    .udp_tx_buf_size     = 0x20000,
    .udp_rx_buf_size     = 0x40000,
    .sb_efficiency       = 4,
    .num_bsd_sessions    = 3,
    .bsd_service_type    = BsdServiceType_User,
};

void net_upscale_hint_input(int dir_x, int dir_y) {
    g_hint_dir_x = dir_x;
    g_hint_dir_y = dir_y;
}

void net_upscale_set_compression(bool compression) {
    g_want_compression = compression;
    // Force a reconnect if a live session negotiated something different —
    // compression is agreed at handshake, so without this the toggle would
    // sit inert until whenever the session next happened to die (or worse,
    // half-apply: the first version of this let the request side react
    // immediately while responses kept the old setting, a real mixed state
    // hit on hardware). The writer notices within one loop pass, tears down,
    // and re-handshakes with the new preference — takes effect in ~1s.
    mutexLock(&g_inflight_lock);
    if (g_conn_alive && g_session_compression != compression)
        g_conn_alive = false;
    mutexUnlock(&g_inflight_lock);
}

bool net_upscale_init(const char *host_port, const char *pairing_code, bool compression) {
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
    g_want_compression = compression;

    if (R_FAILED(socketInitialize(&s_sockCfg))) return false;

    mutexInit(&g_lock);
    mutexInit(&g_inflight_lock);
    g_connected = false;
    g_have_result = false;
    g_send_pending = false;
    g_conn_alive = false;
    g_token = 0;
    g_next_frame_id = 0;
    g_loss_count = 0;
    g_acked_input_id = 0;
    g_held_output_id = 0;
    g_need_out_key = false;
    memset(g_inflight, 0, sizeof(g_inflight));
    g_run = true;

    if (R_FAILED(threadCreate(&g_thread, net_thread_func, NULL, NULL, 0x8000, 0x2C, 1)) ||
        R_FAILED(threadStart(&g_thread))) {
        g_run = false;
        socketExit();
        return false;
    }
    g_ready = true;
    return true;
}

// Must be called with g_lock held. Same content as a recent frame (the SNES
// core's own dupe flag only catches "skipped rendering entirely" — this also
// catches a static scene/paused animation/idle loop that still renders "new"
// frames byte-for-byte identical to a previous one) — skip the network call
// entirely and serve the cached result directly.
static void apply_cache_hit(int slot, uint64_t hash) {
    memcpy(g_result_luma, g_cache[slot].luma, (size_t)g_cache[slot].out_w * g_cache[slot].out_h);
    g_result_w = g_cache[slot].out_w;
    g_result_h = g_cache[slot].out_h;
    g_have_result = true;
    g_result_generation++;
    g_last_result_hash = hash;
}

void net_upscale_submit_frame(const uint8_t *luma, unsigned w, unsigned h) {
    if (!g_ready || w == 0 || h == 0 || w > MAX_W || h > MAX_H) return;
    uint64_t hash = fnv1a(luma, (size_t)w * h);

    mutexLock(&g_lock);
    int slot = cache_find(hash, w, h);
    if (slot >= 0) {
        apply_cache_hit(slot, hash);
        mutexUnlock(&g_lock);
        return;
    }
    memcpy(g_send_luma, luma, (size_t)w * h);
    g_send_w = w; g_send_h = h;
    g_pending_hash = hash;
    g_send_pending = true;
    mutexUnlock(&g_lock);
}

// Like net_upscale_submit_frame, but blocks the CALLING thread (intended:
// called from video_refresh, which libretro invokes synchronously from
// inside retro_run() — so this is what makes retro_run() itself wait on the
// network result when Network Upscale is on, rather than treating it as
// pure background work the game never waits for) until this exact frame's
// result is ready, or timeout_ms elapses.
//
// Recovery on failure: a dead connection, a stalled server, a lost datagram,
// or the in-flight cap all just mean this returns false after timeout_ms —
// never longer, even if the network is completely wedged, so a bad
// connection can never hang the game thread. The caller keeps whatever it
// was already showing (present()'s existing net_upscale_get_result() poll
// still returns the last successful result) — a missed frame is a brief
// visual staleness, never a stall or a crash.
//
// Correctness note: with more than one request in flight (MAX_INFLIGHT), a
// PRIOR frame's request can still be outstanding when a later call times out
// and gives up on it, then submits a new frame. Matching purely on "did
// g_result_generation change" would risk that stale, previously-abandoned
// response landing and being mistaken for the current frame's answer. Hence
// the hash check below: only a result whose g_last_result_hash matches THIS
// call's own request hash is accepted; anything else is a stale straggler,
// silently skipped, and waiting continues (bounded by the same timeout).
//
// out_luma/out_w/out_h are optional (pass NULL for any/all) — most callers
// only care that the *wait* happened before moving on, and already fetch the
// actual result separately via net_upscale_get_result() (e.g. present(),
// which is also the only place that pays the CPU-side recombine cost; not
// worth doing that twice per frame).
bool net_upscale_submit_and_wait(const uint8_t *luma, unsigned w, unsigned h,
                                  unsigned timeout_ms,
                                  const uint8_t **out_luma, unsigned *out_w, unsigned *out_h) {
    if (!g_ready || w == 0 || h == 0 || w > MAX_W || h > MAX_H) return false;
    uint64_t hash = fnv1a(luma, (size_t)w * h);

    mutexLock(&g_lock);
    int slot = cache_find(hash, w, h);
    if (slot >= 0) {
        apply_cache_hit(slot, hash);
        if (out_luma) *out_luma = g_result_luma;
        if (out_w) *out_w = g_result_w;
        if (out_h) *out_h = g_result_h;
        mutexUnlock(&g_lock);
        return true;
    }
    memcpy(g_send_luma, luma, (size_t)w * h);
    g_send_w = w; g_send_h = h;
    g_pending_hash = hash;
    g_send_pending = true;
    unsigned last_checked_gen = g_result_generation;
    mutexUnlock(&g_lock);

    for (unsigned waited_ms = 0; waited_ms < timeout_ms; waited_ms++) {
        svcSleepThread(1000000ULL);  // 1ms

        mutexLock(&g_lock);
        if (g_have_result && g_result_generation != last_checked_gen) {
            last_checked_gen = g_result_generation;
            if (g_last_result_hash == hash) {
                if (out_luma) *out_luma = g_result_luma;
                if (out_w) *out_w = g_result_w;
                if (out_h) *out_h = g_result_h;
                mutexUnlock(&g_lock);
                return true;
            }
            // Someone else's (stale/abandoned) result — not ours, keep waiting.
        }
        mutexUnlock(&g_lock);
    }
    return false;
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

unsigned net_upscale_get_last_rtt_us(void) {
    return g_last_rtt_us;
}

unsigned net_upscale_get_result_generation(void) {
    return g_result_generation;
}

uint64_t net_upscale_get_result_hash(void) {
    // Lock-free read is fine: aligned u64 loads are atomic on aarch64, and
    // this is a display-side gate, not a correctness boundary.
    return g_last_result_hash;
}

void net_upscale_exit(void) {
    if (!g_ready) return;
    g_ready = false;
    g_run = false;
    // The writer thread's own loop joins the reader thread as part of
    // tearing down its current connection cycle before exiting.
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    socketExit();
}
