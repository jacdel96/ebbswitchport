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
    bool     ai_upscale;       // experimental ESPCN super-resolution on the game quad;
                                // GPU-path only, only does anything if a local (unshipped,
                                // see gpu_video.h) weights file is present; default off
    char     net_host[64];        // "ip:port" of the laptop running net_upscale_server.py;
                                    // empty = not configured yet
    char     net_pairing_code[64]; // shared secret for the network-upscale handshake (see
                                    // net_upscale.h) — persisted, unlike net_upscale itself,
                                    // so it doesn't need retyping every session
    bool     net_upscale;      // experimental: offload AI Upscale to the network instead
                                // of the local GPU (see net_upscale.h); mutually exclusive
                                // with ai_upscale — enabling one clears the other; never
                                // persisted, same reasoning as ai_upscale
    bool     net_compression;  // whether the network-upscale response gets zstd-compressed;
                                // negotiated once per connection right after the pairing
                                // handshake (see net_upscale.h) — persisted, since it's a
                                // link preference like net_host, not a live session toggle.
                                // Worth it on WiFi (smaller/faster than the ~2ms compress
                                // cost saves in transit time); not worth it on a wired
                                // gigabit link (see net_upscale_server.py's comment) —
                                // default off, since this project's primary tested setup
                                // is now the direct wired link.
} Settings;

// Fills *out with defaults, then overrides from sdmc:/switch/ebbswitchport/settings.cfg
// if it exists. Missing file/fields silently fall back to defaults.
void settings_load(Settings *out);

// Atomically writes *out to sdmc:/switch/ebbswitchport/settings.cfg.
void settings_save(const Settings *s);
