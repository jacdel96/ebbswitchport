#include "audio.h"

#include <malloc.h>
#include <string.h>
#include <switch.h>

#define OUT_RATE     48000
#define N_BUFFERS    6
#define BUF_FRAMES   800                       // ~one video frame of output
#define BUF_BYTES    0x1000                    // aligned; data_size varies <= this
#define RING_FRAMES  8192                      // FIFO capacity (pow2), ~170ms
#define RING_MASK    (RING_FRAMES - 1)
#define TARGET_FILL  2400                       // desired queued frames (~50ms cushion)

static bool g_ready = false;
static bool g_primed = false;                   // built the initial cushion yet?

// audout output buffers + their in-flight state.
static AudioOutBuffer g_out[N_BUFFERS];
static int16_t *g_mem[N_BUFFERS];
static bool g_inflight[N_BUFFERS];

// Resampled-PCM FIFO (interleaved stereo).
static int16_t g_ring[RING_FRAMES * 2];
static unsigned g_head = 0, g_tail = 0;         // in frames

// Linear-resampler state.
static double g_base_step = 1.0;                // in_rate / OUT_RATE
static double g_pos = 0.0;
static int16_t g_prev_l = 0, g_prev_r = 0;

static inline unsigned ring_available(void) { return (g_head - g_tail) & RING_MASK; }

static inline void ring_push(int16_t l, int16_t r) {
    unsigned next = (g_head + 1) & RING_MASK;
    if (next == g_tail) return;  // full: drop (keeps latency bounded)
    g_ring[g_head * 2] = l;
    g_ring[g_head * 2 + 1] = r;
    g_head = next;
}

static void reclaim(void) {
    AudioOutBuffer *rel;
    u32 count;
    for (int guard = 0; guard < N_BUFFERS * 2; guard++) {
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &count)) || count == 0)
            return;
        for (int i = 0; i < N_BUFFERS; i++)
            if (&g_out[i] == rel) { g_inflight[i] = false; break; }
    }
}

// Push all pending audio into any free audout buffers (partial buffers OK), so
// audout is fed every frame and never starves between submits.
static void pump(void) {
    reclaim();
    // Hold off until we've built a cushion, so playback starts without underrun.
    if (!g_primed) {
        if (ring_available() < TARGET_FILL) return;
        g_primed = true;
    }
    for (int i = 0; i < N_BUFFERS; i++) {
        if (g_inflight[i]) continue;
        unsigned avail = ring_available();
        if (avail == 0) break;
        unsigned n = avail < BUF_FRAMES ? avail : BUF_FRAMES;
        int16_t *dst = g_mem[i];
        for (unsigned f = 0; f < n; f++) {
            dst[f * 2]     = g_ring[g_tail * 2];
            dst[f * 2 + 1] = g_ring[g_tail * 2 + 1];
            g_tail = (g_tail + 1) & RING_MASK;
        }
        g_out[i].data_size = n * 2 * sizeof(int16_t);
        g_out[i].data_offset = 0;
        if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&g_out[i])))
            g_inflight[i] = true;
        else
            break;
    }
}

void audio_init(unsigned in_rate) {
    if (R_FAILED(audoutInitialize())) return;
    audoutStartAudioOut();
    for (int i = 0; i < N_BUFFERS; i++) {
        g_mem[i] = memalign(0x1000, BUF_BYTES);
        memset(g_mem[i], 0, BUF_BYTES);
        g_out[i].next = NULL;
        g_out[i].buffer = g_mem[i];
        g_out[i].buffer_size = BUF_BYTES;
        g_out[i].data_size = BUF_BYTES;
        g_out[i].data_offset = 0;
        g_inflight[i] = false;
    }
    g_base_step = (double)in_rate / OUT_RATE;
    g_pos = 0.0;
    g_primed = false;
    g_ready = true;
}

void audio_submit(const int16_t *stereo, size_t frames) {
    if (!g_ready) return;

    // Dynamic rate control: nudge the resample ratio toward keeping the ring at
    // TARGET_FILL, so we track the 48 kHz consumption rate instead of drifting
    // (the core's ~60.1 Hz vs the display's 60 Hz would otherwise underrun).
    double fill = (double)ring_available();
    double adj = 1.0 + 0.005 * (fill - TARGET_FILL) / TARGET_FILL;
    if (adj < 0.99) adj = 0.99;
    if (adj > 1.01) adj = 1.01;
    double step = g_base_step * adj;

    for (size_t n = 0; n < frames; n++) {
        int16_t cl = stereo[n * 2], cr = stereo[n * 2 + 1];
        while (g_pos < 1.0) {
            int16_t ol = (int16_t)(g_prev_l + (cl - g_prev_l) * g_pos);
            int16_t orr = (int16_t)(g_prev_r + (cr - g_prev_r) * g_pos);
            ring_push(ol, orr);
            g_pos += step;
        }
        g_pos -= 1.0;
        g_prev_l = cl;
        g_prev_r = cr;
    }
    pump();
}

void audio_exit(void) {
    if (!g_ready) return;
    g_ready = false;
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < N_BUFFERS; i++)
        free(g_mem[i]);
}
