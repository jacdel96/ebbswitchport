#include "audio.h"

#include <malloc.h>
#include <string.h>
#include <switch.h>

// Feeding model follows RetroArch's switch_thread_audio.c: a dedicated feeder
// thread owns the audout session and keeps it fed from the ring FIFO, fully
// decoupled from the video loop. The main thread only resamples into the ring,
// so audio keeps playing through frame-time spikes (autosaves, heavy scenes)
// and drains gracefully while the game is paused in the menu.

#define OUT_RATE     48000
#define N_BUFFERS    6
#define BUF_FRAMES   800                       // ~one video frame of output; also the
                                                // practical floor for the buffer_ms setting,
                                                // since it's the feeder's fixed chunk size
#define BUF_BYTES    0x1000                    // aligned; data_size varies <= this
#define RING_FRAMES  8192                      // FIFO capacity (pow2), ~170ms
#define RING_MASK    (RING_FRAMES - 1)

static bool g_ready = false;
static bool g_primed = false;                   // built the initial cushion yet?

// User-configurable buffer cushion (see audio_set_buffer_ms). Guarded by g_lock,
// same as everything else the feeder thread and audio_submit() share.
static unsigned g_prime_fill = 1440;            // ring frames needed before (re)start
static unsigned g_target_total = 1440;          // desired ring+in-flight frames

// audout output buffers + their in-flight state.
static AudioOutBuffer g_out[N_BUFFERS];
static int16_t *g_mem[N_BUFFERS];
static bool g_inflight[N_BUFFERS];
static unsigned g_inflight_frames = 0;          // frames queued inside audout

// Resampled-PCM FIFO (interleaved stereo).
static int16_t g_ring[RING_FRAMES * 2];
static unsigned g_head = 0, g_tail = 0;         // in frames
static unsigned g_stat_drops = 0;               // frames dropped (ring full)
static unsigned g_stat_append_fail = 0;         // audoutAppendAudioOutBuffer errors
static unsigned g_stat_reprimes = 0;            // full drains -> cushion rebuilds
static Result   g_stat_last_err = 0;

// Feeder thread. g_lock guards the ring, in-flight state, and stats.
static Thread g_thread;
static Mutex g_lock;
static volatile bool g_run = false;

// Linear-resampler state (main thread only).
static double g_base_step = 1.0;                // in_rate / OUT_RATE
static double g_pos = 0.0;
static int16_t g_prev_l = 0, g_prev_r = 0;

static inline unsigned ring_available(void) { return (g_head - g_tail) & RING_MASK; }

static inline void ring_push(int16_t l, int16_t r) {
    unsigned next = (g_head + 1) & RING_MASK;
    if (next == g_tail) { g_stat_drops++; return; }  // full: drop (bounds latency)
    g_ring[g_head * 2] = l;
    g_ring[g_head * 2 + 1] = r;
    g_head = next;
}

// Both run on the feeder thread with g_lock held.
static void reclaim_locked(void) {
    AudioOutBuffer *rel;
    u32 count;
    for (int guard = 0; guard < N_BUFFERS * 2; guard++) {
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &count)) || count == 0)
            return;
        for (int i = 0; i < N_BUFFERS; i++)
            if (&g_out[i] == rel) {
                g_inflight[i] = false;
                g_inflight_frames -= rel->data_size / (2 * sizeof(int16_t));
                break;
            }
    }
}

// Feed audout from the ring in uniform BUF_FRAMES chunks. Uniform sizes keep
// the audout queue depth proportional to buffered time; the ring holds any
// sub-buffer remainder. After a complete drain (underrun/pause) we re-prime,
// rebuilding the cushion before restarting instead of dribbling thin buffers.
static void pump_locked(void) {
    if (g_primed && g_inflight_frames == 0 && ring_available() < BUF_FRAMES) {
        g_primed = false;
        g_stat_reprimes++;
    }
    if (!g_primed) {
        if (ring_available() < g_prime_fill) return;
        g_primed = true;
    }
    for (int i = 0; i < N_BUFFERS; i++) {
        if (g_inflight[i]) continue;
        if (ring_available() < BUF_FRAMES) break;
        int16_t *dst = g_mem[i];
        unsigned tail = g_tail;
        for (unsigned f = 0; f < BUF_FRAMES; f++) {
            dst[f * 2]     = g_ring[tail * 2];
            dst[f * 2 + 1] = g_ring[tail * 2 + 1];
            tail = (tail + 1) & RING_MASK;
        }
        g_out[i].data_size = BUF_FRAMES * 2 * sizeof(int16_t);
        g_out[i].data_offset = 0;
        Result rc = audoutAppendAudioOutBuffer(&g_out[i]);
        if (R_FAILED(rc)) {
            // Keep the frames in the ring and retry on the next pass.
            g_stat_append_fail++;
            g_stat_last_err = rc;
            break;
        }
        g_tail = tail;
        g_inflight[i] = true;
        g_inflight_frames += BUF_FRAMES;
    }
}

static void feeder(void *arg) {
    (void)arg;
    while (g_run) {
        mutexLock(&g_lock);
        reclaim_locked();
        pump_locked();
        mutexUnlock(&g_lock);
        svcSleepThread(2000000ULL);  // 2ms; buffers are 16.7ms so this is ample
    }
}

// Shared clamp/derive logic for audio_init's initial value and audio_set_buffer_ms's
// live updates. Caller holds g_lock (or, during audio_init, no thread exists yet to
// race with, so the lock isn't needed there).
static void apply_buffer_ms(unsigned ms) {
    if (ms < AUDIO_BUFFER_MS_MIN) ms = AUDIO_BUFFER_MS_MIN;
    if (ms > AUDIO_BUFFER_MS_MAX) ms = AUDIO_BUFFER_MS_MAX;
    g_target_total = ms * OUT_RATE / 1000;
    // Re-priming waits for half the target cushion (capped below by the feeder's
    // fixed chunk size, since it can't push anything smaller than that anyway) —
    // long enough to avoid immediately underrunning again, short enough that
    // resuming from the pause menu doesn't reintroduce a long silence.
    g_prime_fill = g_target_total / 2;
    if (g_prime_fill < BUF_FRAMES) g_prime_fill = BUF_FRAMES;
}

void audio_set_buffer_ms(unsigned ms) {
    if (!g_ready) return;
    mutexLock(&g_lock);
    apply_buffer_ms(ms);
    mutexUnlock(&g_lock);
}

void audio_init(unsigned in_rate, unsigned buffer_ms) {
    apply_buffer_ms(buffer_ms);
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
    mutexInit(&g_lock);
    g_run = true;
    // Prio just above the main thread, pinned off core 0 (RetroArch uses core 2).
    if (R_FAILED(threadCreate(&g_thread, feeder, NULL, NULL, 0x4000, 0x2B, 2)) ||
        R_FAILED(threadStart(&g_thread))) {
        g_run = false;
        audoutStopAudioOut();
        audoutExit();
        for (int i = 0; i < N_BUFFERS; i++)
            free(g_mem[i]);
        return;
    }
    g_ready = true;
}

void audio_submit(const int16_t *stereo, size_t frames) {
    if (!g_ready) return;

    mutexLock(&g_lock);

    // Dynamic rate control: nudge the resample ratio toward keeping the total
    // buffered audio (ring + queued inside audout) at g_target_total, so we
    // track the 48 kHz consumption rate instead of drifting (the core's
    // ~60.1 Hz vs the display's 60 Hz would otherwise underrun).
    double fill = (double)(ring_available() + g_inflight_frames);
    double adj = 1.0 + 0.005 * (fill - g_target_total) / g_target_total;
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

    mutexUnlock(&g_lock);
}

void audio_stats(AudioStats *out) {
    mutexLock(&g_lock);
    out->ring_frames = ring_available();
    out->inflight_frames = g_inflight_frames;
    out->drops = g_stat_drops;
    out->append_fails = g_stat_append_fail;
    out->reprimes = g_stat_reprimes;
    out->last_err = (unsigned)g_stat_last_err;
    out->primed = g_primed;
    mutexUnlock(&g_lock);
}

void audio_exit(void) {
    if (!g_ready) return;
    g_ready = false;
    g_run = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < N_BUFFERS; i++)
        free(g_mem[i]);
}
