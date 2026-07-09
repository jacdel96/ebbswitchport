// Minimal push-style audio for the native frontend: takes the libretro core's
// interleaved stereo s16 samples at its native rate, linear-resamples to the
// Switch's fixed 48 kHz, and streams them through audout.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// core sample rate (e.g. ~32040 for snes9x) + initial buffer_ms (see audio_set_buffer_ms)
void audio_init(unsigned in_rate, unsigned buffer_ms);
void audio_submit(const int16_t *stereo, size_t frames);  // frames = stereo pairs
void audio_exit(void);

// Retunes the target ring+in-flight cushion (clamped to [AUDIO_BUFFER_MS_MIN,
// AUDIO_BUFFER_MS_MAX]); safe to call at any time after audio_init, including
// live from the settings menu — takes effect on the next audio_submit().
#define AUDIO_BUFFER_MS_MIN 20
#define AUDIO_BUFFER_MS_MAX 100
void audio_set_buffer_ms(unsigned ms);

// Transport health counters, for the in-game menu's diagnostic line.
typedef struct {
    unsigned ring_frames;     // frames waiting in the FIFO
    unsigned inflight_frames; // frames queued inside audout
    unsigned drops;           // frames dropped because the FIFO was full
    unsigned append_fails;    // audoutAppendAudioOutBuffer failures
    unsigned reprimes;        // complete drains (underrun/pause -> rebuild)
    unsigned last_err;        // last audout error code (0 if none)
    bool primed;
} AudioStats;
void audio_stats(AudioStats *out);
