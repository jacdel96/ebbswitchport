// Frontend-wide preferences (hardware acceleration, audio buffer size, HUD),
// persisted to a single file shared by every game NRO built from this source
// (they're device/performance prefs, not per-game data).
#pragma once
#include <stdbool.h>

typedef struct {
    bool     hw_accel;         // render via GPU (deko3d) instead of CPU; applied at next launch;
                                // default off — crashed on real hardware in initial testing
    unsigned audio_buffer_ms;  // audio_set_buffer_ms() target; one of the presets in main.c
    bool     show_hud;         // draw a live FPS/audio-buffer readout while playing
    bool     dynamic_overclock; // temporarily overclock the CPU core on sustained frame
                                 // drops (see main.c's overclock_tick); live, no restart
    unsigned overclock_trigger_dupes; // consecutive dupes that arm a boost
    unsigned overclock_boost_frames;  // how long a boost lasts once (re)armed
    bool     crt_mode;         // scanlines/phosphor mask/vignette on the game
                                // quad; GPU-path only (no-op on CPU path), live
} Settings;

// Fills *out with defaults, then overrides from sdmc:/switch/ebbswitchport/settings.cfg
// if it exists. Missing file/fields silently fall back to defaults.
void settings_load(Settings *out);

// Atomically writes *out to sdmc:/switch/ebbswitchport/settings.cfg.
void settings_save(const Settings *s);
