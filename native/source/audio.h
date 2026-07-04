// Minimal push-style audio for the native frontend: takes the libretro core's
// interleaved stereo s16 samples at its native rate, linear-resamples to the
// Switch's fixed 48 kHz, and streams them through audout.
#pragma once
#include <stddef.h>
#include <stdint.h>

void audio_init(unsigned in_rate);   // core sample rate (e.g. ~32040 for snes9x)
void audio_submit(const int16_t *stereo, size_t frames);  // frames = stereo pairs
void audio_exit(void);
