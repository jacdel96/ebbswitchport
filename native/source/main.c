// ebbswitchport native frontend.
//
// Statically links the snes9x libretro core, loads romfs:/game.sfc, and drives
// a libnx framebuffer from the core's video callback. Input is mapped from the
// Switch pad to the SNES layout; audio goes out via audio.c. ZR opens an
// in-game menu (osd.c) for save/load across 10 manual + 2 automatic state
// slots, plus battery-SRAM persistence and auto-resume from the 1-min auto slot.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "audio.h"
#include "gpu_video.h"
#include "libretro.h"
#include "net_upscale.h"
#include "osd.h"
#include "pixfmt.h"
#include "save_io.h"
#include "settings.h"

#ifndef GAME_ID
#define GAME_ID "game"
#endif
#define SAVE_DIR "sdmc:/switch/ebbswitchport"

// --- globals -----------------------------------------------------------------
static Framebuffer g_fb;
static enum retro_pixel_format g_px_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static u64 g_held = 0;            // Switch buttons currently held (cached each frame)
static void *g_rom = NULL;        // loaded ROM image (kept alive for the core)

// Reusable nearest-neighbor source-column/row index maps (sized to dst rect).
static int *g_map_x = NULL, *g_map_y = NULL;
static unsigned g_map_w = 0, g_map_h = 0;

// The game window inside the 1280x720 framebuffer (aspect-correct 4:3, centered).
#define FB_W 1280
#define FB_H 720
#define DST_H 720
#define DST_W 960
#define DST_X0 ((FB_W - DST_W) / 2)
#define DST_Y0 ((FB_H - DST_H) / 2)

// The last fully-rendered game frame (persistent). video_refresh fills this;
// present() copies it to the real framebuffer. Keeping it around lets the menu
// composite over a frozen frame while the game is paused.
static u32 *g_frame = NULL;

// In-game menu (opened with ZR). Levels: 0 = main, 1 = save-slot list,
// 2 = load-slot list, 3 = settings.
#define SLOT_COUNT 10            // manual slots 0..9
#define LOAD_COUNT (SLOT_COUNT + 2)  // + auto1 + auto10
#define MAIN_COUNT 6             // Resume/Save/Load/Reset/Settings/Exit
#define SETTINGS_COUNT 10        // Hardware Accel/Audio Buffer/Show HUD/Dynamic Overclock/
                                  // OC Trigger/OC Boost/CRT Mode/AI Upscale/
                                  // Network Setup/Network Upscale
static bool g_menu_open = false;
static int g_menu_level = 0;
static int g_menu_sel = 0;

// --- settings ------------------------------------------------------------
static Settings g_settings;
static const unsigned AUDIO_BUFFER_PRESETS[] = {20, 30, 50, 75, 100};
#define AUDIO_PRESET_COUNT (sizeof(AUDIO_BUFFER_PRESETS) / sizeof(AUDIO_BUFFER_PRESETS[0]))
static int g_audio_preset_idx = 1;  // index into AUDIO_BUFFER_PRESETS; default row = 30ms

// Snaps g_settings.audio_buffer_ms to the nearest preset and syncs g_audio_preset_idx —
// used at startup, since a hand-edited settings.cfg could contain a non-preset value.
static void audio_preset_sync(void) {
    unsigned best_diff = ~0u;
    for (unsigned i = 0; i < AUDIO_PRESET_COUNT; i++) {
        unsigned diff = AUDIO_BUFFER_PRESETS[i] > g_settings.audio_buffer_ms
                       ? AUDIO_BUFFER_PRESETS[i] - g_settings.audio_buffer_ms
                       : g_settings.audio_buffer_ms - AUDIO_BUFFER_PRESETS[i];
        if (diff < best_diff) { best_diff = diff; g_audio_preset_idx = i; }
    }
    g_settings.audio_buffer_ms = AUDIO_BUFFER_PRESETS[g_audio_preset_idx];
}

// --- live gameplay HUD (FPS + audio buffer + lag, toggled from Settings) -----
static unsigned g_fps_x10 = 0;     // displayed fps * 10; 0 until the first window closes
static unsigned g_fps_counter = 0;
static u64 g_fps_tick0 = 0;

// A "duped frame" (video_refresh called with data == NULL) is the core
// explicitly telling us this frame is pixel-identical to the last — which is
// exactly what happens on real SNES hardware when the CPU can't finish a
// frame's work in time and the PPU holds the previous image (authentic
// slowdown, not an emulation performance problem). Tracking the fraction of
// recent frames that were dupes gives a live, direct signal for that, rather
// than inferring it indirectly from perceived stutter.
static unsigned g_dupe_count = 0;   // dupes in the current window
static unsigned g_frame_total = 0;  // total video_refresh calls in the current window
static unsigned g_lag_pct = 0;      // last computed dupe percentage, shown in the HUD

// --- dynamic CPU overclock ----------------------------------------------------
// snes9x-libretro's "snes9x_overclock_cycles" core option (handled in
// environ_cb below) gives the emulated 65816 more cycles per frame, which
// helps exactly the mechanism we think is causing dupes: the ROM's own code
// checking a per-frame CPU-cycle budget and skipping updates when it's blown.
// Rather than run overclocked all the time (which trades away hardware-
// accurate timing/audio sync everywhere, including scenes where it's not
// needed), only engage it in short bursts triggered by a run of real dupes,
// and back off once things have been smooth for a while. Both thresholds are
// tunable from the Settings menu (g_settings.overclock_trigger_dupes/
// overclock_boost_frames) so this can be experimented with on hardware.
#define OVERCLOCK_TRIGGER_MIN 1
#define OVERCLOCK_TRIGGER_MAX 30
#define OVERCLOCK_BOOST_MIN   10
#define OVERCLOCK_BOOST_MAX   600
static unsigned g_dupe_streak = 0;
static bool g_overclock_boost = false;
static bool g_overclock_dirty = false;    // tells the core to re-poll variables
static unsigned g_overclock_frames_left = 0;

// Call once per video_refresh(), with whether that frame was a dupe.
static void overclock_tick(bool duped) {
    if (!g_settings.dynamic_overclock) return;  // never triggers; toggling off cancels any active boost (see settings_adjust)
    if (duped) {
        g_dupe_streak++;
        if (g_dupe_streak >= g_settings.overclock_trigger_dupes) {
            if (!g_overclock_boost) { g_overclock_boost = true; g_overclock_dirty = true; }
            g_overclock_frames_left = g_settings.overclock_boost_frames;  // (re)arm while trouble persists
        }
    } else {
        g_dupe_streak = 0;
    }
    if (g_overclock_boost && g_overclock_frames_left > 0 && --g_overclock_frames_left == 0) {
        g_overclock_boost = false;
        g_overclock_dirty = true;
    }
}

// Call once per frame retro_run() actually advances (i.e. not while paused in
// the menu), so the FPS/lag readings reflect real gameplay throughput.
static void fps_tick(void) {
    g_fps_counter++;
    if (g_fps_counter < 30) return;
    u64 now = armGetSystemTick();
    if (g_fps_tick0 != 0) {
        double secs = armTicksToNs(now - g_fps_tick0) / 1e9;
        if (secs > 0.0) g_fps_x10 = (unsigned)(g_fps_counter * 10.0 / secs + 0.5);
    }
    g_fps_tick0 = now;
    g_fps_counter = 0;

    if (g_frame_total > 0) g_lag_pct = g_dupe_count * 100 / g_frame_total;
    g_dupe_count = 0;
    g_frame_total = 0;
}

// Direct measurement of how long retro_run() itself takes, independent of the
// dupe-frame signal above (which some cores, possibly including this one,
// never actually use — a 0% lag reading doesn't rule out real per-frame cost,
// it may just mean the core always hands back fresh pixels regardless of how
// long it took). A call exceeding ~16.7ms is by itself missing the 60fps
// frame budget, dupes or not.
static unsigned long long g_run_us_sum = 0;
static unsigned g_run_us_count = 0;
static unsigned g_run_us_window_max = 0;
static unsigned g_run_avg_us = 0;   // last computed window average, shown in the HUD
static unsigned g_run_max_us = 0;   // last computed window max, shown in the HUD

// Call once per retro_run(), with how long that call took in microseconds.
static void run_timing_tick(unsigned us) {
    g_run_us_sum += us;
    g_run_us_count++;
    if (us > g_run_us_window_max) g_run_us_window_max = us;
    if (g_run_us_count < 30) return;
    g_run_avg_us = (unsigned)(g_run_us_sum / g_run_us_count);
    g_run_max_us = g_run_us_window_max;
    g_run_us_sum = 0;
    g_run_us_count = 0;
    g_run_us_window_max = 0;
}

// Same windowing as run_timing_tick, for the experimental AI (ESPCN) upscale
// pass — see gpu_video_get_ai_upscale_us's comment. Only ticked on frames it
// actually ran, so the average reflects real dispatch cost, not diluted by
// frames where it was skipped (menu closed vs open, resolution out of range).
static unsigned long long g_ai_us_sum = 0;
static unsigned g_ai_us_count = 0;
static unsigned g_ai_us_window_max = 0;
static unsigned g_ai_avg_us = 0;
static unsigned g_ai_max_us = 0;

static void ai_timing_tick(unsigned us) {
    g_ai_us_sum += us;
    g_ai_us_count++;
    if (us > g_ai_us_window_max) g_ai_us_window_max = us;
    if (g_ai_us_count < 30) return;
    g_ai_avg_us = (unsigned)(g_ai_us_sum / g_ai_us_count);
    g_ai_max_us = g_ai_us_window_max;
    g_ai_us_sum = 0;
    g_ai_us_count = 0;
    g_ai_us_window_max = 0;
}

// Same windowing again, for the experimental network AI-upscale offload
// (see net_upscale.h) — only ticked on frames where a new result actually
// arrived, mirroring ai_timing_tick's reasoning.
static unsigned long long g_net_us_sum = 0;
static unsigned g_net_us_count = 0;
static unsigned g_net_us_window_max = 0;
static unsigned g_net_avg_us = 0;
static unsigned g_net_max_us = 0;

static void net_timing_tick(unsigned us) {
    g_net_us_sum += us;
    g_net_us_count++;
    if (us > g_net_us_window_max) g_net_us_window_max = us;
    if (g_net_us_count < 30) return;
    g_net_avg_us = (unsigned)(g_net_us_sum / g_net_us_count);
    g_net_max_us = g_net_us_window_max;
    g_net_us_sum = 0;
    g_net_us_count = 0;
    g_net_us_window_max = 0;
}

// --- rendering backend selection ---------------------------------------------
// GPU (deko3d) path state — populated only when g_use_gpu is true.
static bool g_use_gpu = false;
static u32 *g_native_frame = NULL;      // RGBA8, core's native res, GPU-path upload source
static unsigned g_native_cap = 0;       // g_native_frame capacity, in pixels
static unsigned g_native_w = 0, g_native_h = 0;
static u32 *g_menu_frame = NULL;        // 1280x720 backdrop for the GPU-path menu overlay
static u32 *g_hud_frame = NULL;         // small HUD_W x HUD_H panel for the GPU-path HUD
static PixelLut g_pixlut = {0};         // shared 16bpp->RGBA8 LUT (CPU and GPU paths)

// --- network AI-upscale state (see net_upscale.h) -----------------------------
static bool g_net_initialized = false;  // net_upscale_init has been called this session
static uint8_t *g_luma_buf = NULL;      // scratch: luma extracted from g_native_frame
static unsigned g_luma_cap = 0;
static unsigned g_net_last_generation = 0;  // last net_upscale_get_result_generation()
                                             // we ticked timing for — see net_timing_tick

// --- libretro callbacks ------------------------------------------------------
static void video_refresh(const void *data, unsigned width, unsigned height,
                          size_t pitch) {
    g_frame_total++;
    if (!data) { g_dupe_count++; overclock_tick(true); return; }  // duped frame — see g_lag_pct's comment
    overclock_tick(false);

    if (g_use_gpu) {
        unsigned need = width * height;
        if (need > g_native_cap) {
            free(g_native_frame);
            g_native_frame = malloc((size_t)need * sizeof(u32));
            g_native_cap = need;
        }
        pixfmt_convert_to_rgba8(g_native_frame, data, width, height, pitch, g_px_fmt, &g_pixlut);
        g_native_w = width; g_native_h = height;
        gpu_video_upload_frame(g_native_frame, width, height);

        if (g_settings.net_upscale && g_net_initialized) {
            // Luma-only (see net_upscale.h) — same convention as espcn1_comp.glsl's
            // luma extraction (matches the network's own model, which was only ever
            // trained on the Y channel).
            unsigned need = width * height;
            if (need > g_luma_cap) {
                free(g_luma_buf);
                g_luma_buf = malloc(need);
                g_luma_cap = need;
            }
            if (g_luma_buf) {
                for (unsigned i = 0; i < need; i++) {
                    u32 px = g_native_frame[i];
                    float r = (float)(px & 0xFF), g = (float)((px >> 8) & 0xFF), b = (float)((px >> 16) & 0xFF);
                    float y = 0.299f * r + 0.587f * g + 0.114f * b;
                    g_luma_buf[i] = (uint8_t)(y < 0.0f ? 0.0f : (y > 255.0f ? 255.0f : y));
                }
                net_upscale_submit_frame(g_luma_buf, width, height);
            }
        }
        return;
    }

    // Rebuild scale maps if the core's output geometry changed.
    if (width != g_map_w || height != g_map_h) {
        free(g_map_x); free(g_map_y);
        g_map_x = malloc(sizeof(int) * DST_W);
        g_map_y = malloc(sizeof(int) * DST_H);
        for (unsigned dx = 0; dx < DST_W; dx++) g_map_x[dx] = dx * width / DST_W;
        for (unsigned dy = 0; dy < DST_H; dy++) g_map_y[dy] = dy * height / DST_H;
        g_map_w = width; g_map_h = height;
    }

    const int bpp = (g_px_fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
    if (bpp == 2 && g_pixlut.fmt != g_px_fmt) pixlut_rebuild(&g_pixlut, g_px_fmt);

    // Consecutive dst rows usually map to the same src row (720/224 ~ 3.2x),
    // so convert each src row once and memcpy the repeats.
    int prev_sy = -1;
    for (unsigned dy = 0; dy < DST_H; dy++) {
        u32 *dst_row = g_frame + (size_t)(DST_Y0 + dy) * FB_W + DST_X0;
        int sy = g_map_y[dy];
        if (sy == prev_sy) {
            memcpy(dst_row, dst_row - FB_W, DST_W * sizeof(u32));
            continue;
        }
        prev_sy = sy;
        const u8 *src_row = (const u8 *)data + (size_t)sy * pitch;
        if (bpp == 2) {
            const u16 *s = (const u16 *)src_row;
            for (unsigned dx = 0; dx < DST_W; dx++)
                dst_row[dx] = g_pixlut.table[s[g_map_x[dx]]];
        } else {
            const u32 *s = (const u32 *)src_row;
            for (unsigned dx = 0; dx < DST_W; dx++)
                dst_row[dx] = xrgb8888_to_rgba(s[g_map_x[dx]]);
        }
    }
}

static bool environ_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format fmt = *(const enum retro_pixel_format *)data;
        if (fmt == RETRO_PIXEL_FORMAT_RGB565 ||
            fmt == RETRO_PIXEL_FORMAT_XRGB8888 ||
            fmt == RETRO_PIXEL_FORMAT_0RGB1555) {
            g_px_fmt = fmt;
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = "sdmc:/switch/ebbswitchport";
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        // Only the dynamic-overclock knob is actively driven (see
        // overclock_tick); every other core variable keeps the core's default.
        if (var->key && strcmp(var->key, "snes9x_overclock_cycles") == 0) {
            var->value = g_overclock_boost ? "max" : "disabled";
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        // The core polls this every frame and only re-reads variables (via
        // GET_VARIABLE, above) on the frame we report a change — this is that
        // one-shot signal, cleared immediately after being read.
        *(bool *)data = g_overclock_dirty;
        g_overclock_dirty = false;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        return true;   // accept, ignore
    default:
        return false;
    }
}

static void input_poll(void) { /* pad is polled once per frame in main() */ }

static int16_t input_state(unsigned port, unsigned device, unsigned index,
                           unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    // SNES buttons keep Nintendo's letter/position mapping on the Switch pad.
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A:      return (g_held & HidNpadButton_A)     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_B:      return (g_held & HidNpadButton_B)     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_X:      return (g_held & HidNpadButton_X)     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_Y:      return (g_held & HidNpadButton_Y)     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_START:  return (g_held & HidNpadButton_Plus)  ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return (g_held & HidNpadButton_Minus) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_L:      return (g_held & HidNpadButton_L)     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_R:      return (g_held & HidNpadButton_R)     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_UP:
        return (g_held & (HidNpadButton_Up | HidNpadButton_StickLUp)) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:
        return (g_held & (HidNpadButton_Down | HidNpadButton_StickLDown)) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:
        return (g_held & (HidNpadButton_Left | HidNpadButton_StickLLeft)) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:
        return (g_held & (HidNpadButton_Right | HidNpadButton_StickLRight)) ? 1 : 0;
    default: return 0;
    }
}

// Audio: hand the core's samples to the resampler/audout module.
static void audio_sample(int16_t l, int16_t r) {
    int16_t frame[2] = {l, r};
    audio_submit(frame, 1);
}
static size_t audio_batch(const int16_t *data, size_t frames) {
    audio_submit(data, frames);
    return frames;
}

// --- ROM loading -------------------------------------------------------------
static bool load_rom(struct retro_game_info *gi) {
    FILE *f = fopen("romfs:/game.sfc", "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_rom = malloc(size);
    if (!g_rom || fread(g_rom, 1, size, f) != (size_t)size) {
        fclose(f); return false;
    }
    fclose(f);
    gi->path = "romfs:/game.sfc";
    gi->data = g_rom;
    gi->size = size;
    gi->meta = NULL;
    return true;
}

// --- persistence (SRAM battery + save states) --------------------------------
static void ensure_save_dir(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir(SAVE_DIR, 0777);
}

static void save_path(char *out, size_t n, const char *ext) {
    snprintf(out, n, "%s/%s.%s", SAVE_DIR, GAME_ID, ext);
}

// Load battery-backed SRAM from disk into the core's live memory.
static void sram_load(void) {
    void *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size) return;
    char path[256];
    save_path(path, sizeof(path), "srm");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fread(mem, 1, size, f);
    fclose(f);
}

// Snapshot the core's live SRAM and queue it for an off-main-thread write
// (save_io.c) — capturing the copy here, on the main thread, is required
// since it's the only thread allowed to touch the libretro core; the actual
// SD-card write happens later, off this thread, so it can't stall audio.
static void sram_save(void) {
    void *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size) return;
    void *copy = malloc(size);
    if (!copy) return;
    memcpy(copy, mem, size);
    char path[256];
    save_path(path, sizeof(path), "srm");
    save_io_write_async(path, copy, size);
}

// Serialize the whole machine and queue it for an off-main-thread write to
// <game>.<ext> (see sram_save's comment — same reasoning applies here).
static void state_save(const char *ext) {
    size_t size = retro_serialize_size();
    if (!size) return;
    void *buf = malloc(size);
    if (!buf) return;
    if (retro_serialize(buf, size)) {
        char path[256];
        save_path(path, sizeof(path), ext);
        save_io_write_async(path, buf, size);
    } else {
        free(buf);
    }
}

// Restore a machine snapshot from <game>.<ext>. Returns true if a state was
// present and successfully loaded.
static bool state_load(const char *ext) {
    char path[256];
    save_path(path, sizeof(path), ext);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(size);
    bool ok = buf && fread(buf, 1, size, f) == (size_t)size &&
              retro_unserialize(buf, size);
    free(buf);
    fclose(f);
    return ok;
}

// Map a Load-list index to its state extension: 0..9 -> manual slots,
// 10 -> 1-min auto, 11 -> 10-min auto.
static void load_ext(int i, char *buf, size_t n) {
    if (i < SLOT_COUNT)      snprintf(buf, n, "slot%d", i);
    else if (i == SLOT_COUNT) snprintf(buf, n, "auto1");
    else                      snprintf(buf, n, "auto10");
}

static bool state_exists(const char *ext) {
    char path[256];
    save_path(path, sizeof(path), ext);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// Shows the on-screen keyboard with the given header/guide/initial text;
// returns true and fills `out` (up to out_size-1 bytes + NUL) if the user
// confirmed, false (out left untouched) if they backed out. Blocks until the
// applet closes — fine here since it's only ever invoked from the paused
// Settings menu, never during gameplay.
static bool text_entry(const char *header, const char *guide, const char *initial,
                        char *out, size_t out_size) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, guide);
    if (initial && initial[0]) swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, (u32)(out_size - 1));

    char buf[64];
    bool ok = R_SUCCEEDED(swkbdShow(&kbd, buf, sizeof(buf)));
    swkbdClose(&kbd);
    if (!ok) return false;
    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

// --- settings menu row helpers ------------------------------------------------
// Adjusts the settings row `sel` by one step (dir = +1/-1), applies it live
// where that's safe (audio buffer, HUD), and persists immediately.
static void settings_adjust(int sel, int dir) {
    if (sel == 0) {
        g_settings.hw_accel = !g_settings.hw_accel;      // takes effect next launch
    } else if (sel == 1) {
        g_audio_preset_idx = (g_audio_preset_idx + dir + AUDIO_PRESET_COUNT) % AUDIO_PRESET_COUNT;
        g_settings.audio_buffer_ms = AUDIO_BUFFER_PRESETS[g_audio_preset_idx];
        audio_set_buffer_ms(g_settings.audio_buffer_ms);   // live, no restart needed
    } else if (sel == 2) {
        g_settings.show_hud = !g_settings.show_hud;        // live
    } else if (sel == 3) {
        g_settings.dynamic_overclock = !g_settings.dynamic_overclock;  // live
        if (!g_settings.dynamic_overclock) {
            // Don't leave a boost stuck engaged after turning the feature off.
            g_dupe_streak = 0;
            g_overclock_frames_left = 0;
            if (g_overclock_boost) { g_overclock_boost = false; g_overclock_dirty = true; }
        }
    } else if (sel == 4) {
        int v = (int)g_settings.overclock_trigger_dupes + dir;
        if (v < OVERCLOCK_TRIGGER_MIN) v = OVERCLOCK_TRIGGER_MIN;
        if (v > OVERCLOCK_TRIGGER_MAX) v = OVERCLOCK_TRIGGER_MAX;
        g_settings.overclock_trigger_dupes = (unsigned)v;  // live, read fresh by overclock_tick
    } else if (sel == 5) {
        int v = (int)g_settings.overclock_boost_frames + dir * 10;
        if (v < OVERCLOCK_BOOST_MIN) v = OVERCLOCK_BOOST_MIN;
        if (v > OVERCLOCK_BOOST_MAX) v = OVERCLOCK_BOOST_MAX;
        g_settings.overclock_boost_frames = (unsigned)v;   // live, read fresh by overclock_tick
    } else if (sel == 6) {
        g_settings.crt_mode = !g_settings.crt_mode;        // live (GPU path only)
        gpu_video_set_crt(g_settings.crt_mode);
    } else if (sel == 7) {
        // Refuse to flip on when it can't actually run — see the Settings
        // row's "N/A" text for why. Avoids a setting that reads "On" while
        // silently doing nothing (or, worse, reading "On" from a save file
        // on a build/device where it never had a chance to become available).
        if (g_use_gpu && gpu_video_ai_upscale_available()) {
            g_settings.ai_upscale = !g_settings.ai_upscale;
            gpu_video_set_ai_upscale(g_settings.ai_upscale);
            // Mutually exclusive with the network path — both write the same
            // GPU output slot (see gpu_video.h).
            if (g_settings.ai_upscale && g_settings.net_upscale) {
                g_settings.net_upscale = false;
                gpu_video_set_network_upscale(false);
            }
        }
    } else if (sel == 8) {
        // Network Setup: enter the laptop's "ip:port" and the shared pairing
        // code (see native/tools/net_upscale_server.py). Only takes effect
        // the next time Network Upscale is turned on — doesn't reconnect an
        // already-running session.
        char host[64];
        if (text_entry("Laptop address", "e.g. 192.168.1.42:9876",
                        g_settings.net_host, host, sizeof(host))) {
            strncpy(g_settings.net_host, host, sizeof(g_settings.net_host) - 1);
            g_settings.net_host[sizeof(g_settings.net_host) - 1] = '\0';
        }
        char code[64];
        if (text_entry("Pairing code", "must match the laptop's --pairing-code",
                        g_settings.net_pairing_code, code, sizeof(code))) {
            strncpy(g_settings.net_pairing_code, code, sizeof(g_settings.net_pairing_code) - 1);
            g_settings.net_pairing_code[sizeof(g_settings.net_pairing_code) - 1] = '\0';
        }
    } else if (sel == 9) {
        // Refuse to enable without a configured host/pairing code, and
        // mutually exclusive with the local path (see sel==7's comment).
        if (g_use_gpu && g_settings.net_host[0] && g_settings.net_pairing_code[0]) {
            g_settings.net_upscale = !g_settings.net_upscale;
            if (g_settings.net_upscale) {
                if (!g_net_initialized) {
                    g_net_initialized = net_upscale_init(g_settings.net_host, g_settings.net_pairing_code);
                }
                if (g_settings.ai_upscale) {
                    g_settings.ai_upscale = false;
                    gpu_video_set_ai_upscale(false);
                }
            }
            gpu_video_set_network_upscale(g_settings.net_upscale && g_net_initialized);
        }
    }
    settings_save(&g_settings);
}

// --- in-game menu ------------------------------------------------------------
static void draw_menu(u32 *fb, u32 stride) {
    const int scale = 3;
    const int lh = 8 * scale + 12;
    int count = g_menu_level == 0 ? MAIN_COUNT
              : g_menu_level == 1 ? SLOT_COUNT
              : g_menu_level == 2 ? LOAD_COUNT : SETTINGS_COUNT;
    const char *title = g_menu_level == 0 ? "MENU"
                       : g_menu_level == 1 ? "SAVE STATE"
                       : g_menu_level == 2 ? "LOAD STATE" : "SETTINGS";

    osd_rect(fb, stride, FB_H, 0, 0, FB_W, FB_H, 0xB0000000u);  // dim the game
    int pw = 700, ph = lh * (count + 2) + 40;
    int px = (FB_W - pw) / 2, py = (FB_H - ph) / 2;
    osd_rect(fb, stride, FB_H, px, py, pw, ph, 0xF0181818u);    // panel
    osd_rect(fb, stride, FB_H, px, py, pw, 4, 0xFF66CCFFu);     // accent bar

    int tx = px + 40, ty = py + 28;
    osd_text(fb, stride, FB_H, tx, ty, 0xFF66CCFFu, scale, title);
    ty += lh + 10;

    for (int i = 0; i < count; i++) {
        char line[96];  // wide enough for a 16-char padded label + a 63-char host string
        if (g_menu_level == 0) {
            const char *m[] = {"Resume", "Save", "Load", "Reset", "Settings", "Exit"};
            snprintf(line, sizeof(line), "%s", m[i]);
        } else if (g_menu_level == 3) {
            if (i == 0) snprintf(line, sizeof(line), "%-16s %s", "Hardware Accel",
                                  g_settings.hw_accel ? "On" : "Off");
            else if (i == 1) snprintf(line, sizeof(line), "%-16s %ums", "Audio Buffer",
                                       g_settings.audio_buffer_ms);
            else if (i == 2) snprintf(line, sizeof(line), "%-16s %s", "Show HUD",
                                       g_settings.show_hud ? "On" : "Off");
            else if (i == 3) snprintf(line, sizeof(line), "%-16s %s", "Dynamic Overclock",
                                       g_settings.dynamic_overclock ? "On" : "Off");
            else if (i == 4) snprintf(line, sizeof(line), "%-16s %u frames", "OC Trigger",
                                       g_settings.overclock_trigger_dupes);
            else if (i == 5) snprintf(line, sizeof(line), "%-16s %u frames", "OC Boost",
                                       g_settings.overclock_boost_frames);
            else if (i == 6) snprintf(line, sizeof(line), "%-16s %s", "CRT Mode",
                                       g_settings.crt_mode ? "On" : "Off");
            else if (i == 7 && !g_use_gpu) snprintf(line, sizeof(line), "%-16s %s", "AI Upscale",
                                            "N/A (needs GPU accel)");
            else if (i == 7 && !gpu_video_ai_upscale_available()) snprintf(line, sizeof(line), "%-16s %s",
                                            "AI Upscale", "N/A (no weights)");
            else if (i == 7) snprintf(line, sizeof(line), "%-16s %s", "AI Upscale",
                          g_settings.ai_upscale ? "On" : "Off");
            else if (i == 8) snprintf(line, sizeof(line), "%-16s %.20s", "Network Setup",
                          g_settings.net_host[0] ? g_settings.net_host : "(not configured)");
            else if (i == 9 && !g_use_gpu) snprintf(line, sizeof(line), "%-16s %s",
                                            "Network Upscale", "N/A (needs GPU accel)");
            else if (i == 9 && (!g_settings.net_host[0] || !g_settings.net_pairing_code[0]))
                snprintf(line, sizeof(line), "%-16s %s", "Network Upscale", "N/A (run Network Setup)");
            else snprintf(line, sizeof(line), "%-16s %s", "Network Upscale",
                          g_settings.net_upscale ? (net_upscale_connected() ? "On" : "On (connecting)") : "Off");
        } else {
            char ext[16];
            const char *name;
            char nbuf[24];
            if (g_menu_level == 1) {         // save: manual slots only
                snprintf(ext, sizeof(ext), "slot%d", i);
                snprintf(nbuf, sizeof(nbuf), "Slot %d", i);
                name = nbuf;
            } else {                          // load: slots + autos
                load_ext(i, ext, sizeof(ext));
                if (i < SLOT_COUNT) snprintf(nbuf, sizeof(nbuf), "Slot %d", i);
                else if (i == SLOT_COUNT) snprintf(nbuf, sizeof(nbuf), "Auto 1 min");
                else snprintf(nbuf, sizeof(nbuf), "Auto 10 min");
                name = nbuf;
            }
            snprintf(line, sizeof(line), "%-12s %s", name,
                     state_exists(ext) ? "[saved]" : "[empty]");
        }
        u32 col = (i == g_menu_sel) ? 0xFF00FFFFu : 0xFFFFFFFFu;
        if (i == g_menu_sel) osd_text(fb, stride, FB_H, px + 14, ty, col, scale, ">");
        osd_text(fb, stride, FB_H, tx, ty, col, scale, line);
        ty += lh;
    }

    if (g_menu_level == 3) {
        osd_text(fb, stride, FB_H, tx, py + ph - 26, 0xFF888888u, 2,
                 "LEFT/RIGHT change   B back   * Hardware Accel needs a restart");
    } else {
        osd_text(fb, stride, FB_H, tx, py + ph - 26, 0xFF888888u, 2,
                 "UP/DOWN move   A select   B back   ZR close");
    }

    // Audio transport health, for chasing sound dropouts in the field. Note this
    // necessarily reads low/draining: opening the menu pauses retro_run(), so no
    // new audio is being submitted while you're looking at it (see the live HUD,
    // toggled from Settings, for the buffer level during actual gameplay).
    AudioStats as;
    audio_stats(&as);
    char diag[96];
    snprintf(diag, sizeof(diag),
             "audio: buf %ums  drop %u  fail %u (0x%x)  rebuild %u",
             (as.ring_frames + as.inflight_frames) / 48,
             as.drops, as.append_fails, as.last_err, as.reprimes);
    osd_text(fb, stride, FB_H, px + 14, py + ph + 10, 0xFF888888u, 2, diag);
}

// Draws the small live FPS/audio-buffer readout in the corner during gameplay
// (menu closed). Same osd_rect/osd_text primitives as draw_menu, just a much
// smaller panel that doesn't dim or pause anything. `canvas_h` lets this be
// reused both on the real 1280x720 framebuffer (CPU path) and on the small,
// tightly-sized HUD texture built for the GPU path.
static void draw_hud(u32 *fb, u32 stride, u32 canvas_h, int x0, int y0) {
    AudioStats as;
    audio_stats(&as);
    char line1[80], line2[48], line3[48];
    bool ai_active = g_use_gpu && gpu_video_ai_upscale_active();
    bool net_active = g_use_gpu && g_settings.net_upscale && g_net_initialized;
    // g_use_gpu reflects the backend actually running this session (settings
    // hw_accel is only the *request* — gpu_video_init may have failed and
    // silently fallen back to CPU, see main()'s init), not just the setting.
    // Resolution: the GPU path tracks dock/handheld live (gpu_video.c); the
    // CPU path stays fixed at FB_W x FB_H always (see settings.h).
    unsigned res_w = FB_W, res_h = FB_H;
    if (g_use_gpu) gpu_video_get_resolution(&res_w, &res_h);
    snprintf(line1, sizeof(line1), "%s %ux%u  FPS %u.%u  AUDIO %ums  LAG %u%%%s",
             g_use_gpu ? "GPU" : "CPU", res_w, res_h,
             g_fps_x10 / 10, g_fps_x10 % 10, (as.ring_frames + as.inflight_frames) / 48,
             g_lag_pct, g_overclock_boost ? "  OC" : "");
    // Direct retro_run() timing — see run_timing_tick's comment for why this
    // exists alongside (and is more trustworthy than) LAG %.
    unsigned avg_x10 = g_run_avg_us / 100, max_x10 = g_run_max_us / 100;  // tenths of a ms
    snprintf(line2, sizeof(line2), "RUN avg %u.%ums  max %u.%ums",
             avg_x10 / 10, avg_x10 % 10, max_x10 / 10, max_x10 % 10);
    // Experimental AI (ESPCN) upscale timing — only shown while it's actually
    // running (GPU path, weights present, enabled, resolution in range).
    // Mutually exclusive with the network path (see settings_adjust), so at
    // most one of these two ever applies.
    if (ai_active) {
        unsigned ai_avg_x10 = g_ai_avg_us / 100, ai_max_x10 = g_ai_max_us / 100;
        snprintf(line3, sizeof(line3), "AI avg %u.%ums  max %u.%ums",
                 ai_avg_x10 / 10, ai_avg_x10 % 10, ai_max_x10 / 10, ai_max_x10 % 10);
    } else if (net_active) {
        unsigned net_avg_x10 = g_net_avg_us / 100, net_max_x10 = g_net_max_us / 100;
        snprintf(line3, sizeof(line3), "NET avg %u.%ums  max %u.%ums",
                 net_avg_x10 / 10, net_avg_x10 % 10, net_max_x10 / 10, net_max_x10 % 10);
    }

    bool line3_active = ai_active || net_active;
    const int scale = 2;
    int lh = 8 * scale + 6;
    int w1 = osd_text_w(line1, scale), w2 = osd_text_w(line2, scale);
    int w3 = line3_active ? osd_text_w(line3, scale) : 0;
    int wmax = w1 > w2 ? w1 : w2;
    if (w3 > wmax) wmax = w3;
    int lines = line3_active ? 3 : 2;
    int w = wmax + 24, h = lh * lines + 12;
    osd_rect(fb, stride, canvas_h, x0, y0, w, h, 0xFF181818u);
    osd_text(fb, stride, canvas_h, x0 + 12, y0 + 6, 0xFFFFFFFFu, scale, line1);
    osd_text(fb, stride, canvas_h, x0 + 12, y0 + 6 + lh, 0xFFFFFFFFu, scale, line2);
    if (line3_active) osd_text(fb, stride, canvas_h, x0 + 12, y0 + 6 + lh * 2, 0xFFFFFFFFu, scale, line3);
}

// GPU-path only: builds the paused-menu backdrop (native-res frame nearest-
// upscaled into the game window + draw_menu on top) into a full 1280x720
// buffer, uploaded once as an opaque overlay replacement (see gpu_video.h).
static void build_menu_frame(u32 *out) {
    for (int i = 0; i < FB_W * FB_H; i++) out[i] = 0xFF000000u;  // pillarbox bars
    if (g_native_w && g_native_h)
        nn_upscale_rgba(out, FB_W, DST_X0, DST_Y0, DST_W, DST_H,
                         g_native_frame, g_native_w, g_native_h);
    draw_menu(out, FB_W);
}

// GPU-path only: HUD panel sized to its own small buffer (not the full
// 1280x720 canvas), composited as a small quad by gpu_video.c — much cheaper
// to re-upload every gameplay frame than the full-screen menu backdrop.
// HUD_W/HUD_H come from gpu_video.h so both sides of the upload agree on size.
static void build_hud_frame(u32 *out) {
    draw_hud(out, HUD_W, HUD_H, 0, 0);
}

// Present the last game frame (+ menu overlay / HUD if applicable) to the display.
static void present(void) {
    if (g_use_gpu) {
        if (g_menu_open) {
            build_menu_frame(g_menu_frame);
            gpu_video_set_overlay(g_menu_frame);
        } else {
            gpu_video_set_overlay(NULL);
            if (g_settings.show_hud) {
                build_hud_frame(g_hud_frame);
                gpu_video_set_hud(g_hud_frame, HUD_W, HUD_H);
            } else {
                gpu_video_set_hud(NULL, 0, 0);
            }
        }
        if (g_settings.net_upscale && g_net_initialized) {
            const uint8_t *result_luma;
            unsigned rw, rh;
            if (net_upscale_get_result(&result_luma, &rw, &rh)) {
                gpu_video_upload_network_result(result_luma, rw, rh);
                unsigned gen = net_upscale_get_result_generation();
                if (gen != g_net_last_generation) {
                    g_net_last_generation = gen;
                    net_timing_tick(net_upscale_get_last_rtt_us());
                }
            }
        }
        bool ai_ran = gpu_video_ai_upscale_active();
        gpu_video_present();
        if (ai_ran) ai_timing_tick(gpu_video_get_ai_upscale_us());
        return;
    }
    u32 stride;
    u32 *out = (u32 *)framebufferBegin(&g_fb, &stride);
    const u32 row_px = stride / sizeof(u32);
    for (int y = 0; y < FB_H; y++)
        memcpy(out + (size_t)y * row_px, g_frame + (size_t)y * FB_W,
               FB_W * sizeof(u32));
    if (g_menu_open) draw_menu(out, row_px);
    else if (g_settings.show_hud) draw_hud(out, row_px, FB_H, 16, 16);
    framebufferEnd(&g_fb);
}

// Handle a frame of input while the menu is open.
static void menu_input(u64 down, bool *quit) {
    int count = g_menu_level == 0 ? MAIN_COUNT
              : g_menu_level == 1 ? SLOT_COUNT
              : g_menu_level == 2 ? LOAD_COUNT : SETTINGS_COUNT;
    if (down & (HidNpadButton_Down | HidNpadButton_StickLDown))
        g_menu_sel = (g_menu_sel + 1) % count;
    if (down & (HidNpadButton_Up | HidNpadButton_StickLUp))
        g_menu_sel = (g_menu_sel + count - 1) % count;
    if (g_menu_level == 3) {
        if (down & (HidNpadButton_Left | HidNpadButton_StickLLeft)) settings_adjust(g_menu_sel, -1);
        if (down & (HidNpadButton_Right | HidNpadButton_StickLRight)) settings_adjust(g_menu_sel, +1);
    }
    if (down & HidNpadButton_ZR) { g_menu_open = false; return; }  // toggle close
    if (down & HidNpadButton_B) {
        if (g_menu_level == 0) g_menu_open = false;
        else { g_menu_level = 0; g_menu_sel = 0; }
        return;
    }
    if (down & HidNpadButton_A) {
        if (g_menu_level == 0) {
            if (g_menu_sel == 0) g_menu_open = false;                       // Resume
            else if (g_menu_sel == 1) { g_menu_level = 1; g_menu_sel = 0; } // Save
            else if (g_menu_sel == 2) { g_menu_level = 2; g_menu_sel = 0; } // Load
            else if (g_menu_sel == 3) {                                     // Reset
                retro_reset();
                g_menu_open = false;
            }
            else if (g_menu_sel == 4) { g_menu_level = 3; g_menu_sel = 0; } // Settings
            else *quit = true;                                             // Exit
        } else if (g_menu_level == 1) {
            char ext[16];
            snprintf(ext, sizeof(ext), "slot%d", g_menu_sel);
            state_save(ext);
            g_menu_open = false;
        } else if (g_menu_level == 2) {
            char ext[16];
            load_ext(g_menu_sel, ext, sizeof(ext));
            state_load(ext);
            g_menu_open = false;
        } else {
            settings_adjust(g_menu_sel, +1);   // A cycles, same as RIGHT
        }
    }
}

int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    romfsInit();
    save_io_init();

    settings_load(&g_settings);
    audio_preset_sync();

    // GPU rendering is chosen once, at startup: gpu_video_init and the CPU
    // Framebuffer both want exclusive ownership of the same NWindow, so there's
    // no live swap between them (see settings.h — hw_accel takes effect on the
    // next launch). Any failure inside gpu_video_init falls back to the CPU
    // path automatically, so an unproven GPU path can never leave the app
    // unable to boot.
    NWindow *win = nwindowGetDefault();
    g_use_gpu = g_settings.hw_accel && gpu_video_init(win);
    if (g_use_gpu) {
        g_menu_frame = malloc((size_t)FB_W * FB_H * sizeof(u32));
        g_hud_frame = malloc((size_t)HUD_W * HUD_H * sizeof(u32));
        gpu_video_set_crt(g_settings.crt_mode);
        gpu_video_set_ai_upscale(g_settings.ai_upscale);
    } else {
        framebufferCreate(&g_fb, win, FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2);
        framebufferMakeLinear(&g_fb);

        // Persistent frame (opaque black to start, incl. the side pillar-bars).
        g_frame = malloc((size_t)FB_W * FB_H * sizeof(u32));
        for (int i = 0; i < FB_W * FB_H; i++) g_frame[i] = 0xFF000000u;
    }

    retro_set_environment(environ_cb);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_game_info gi;
    if (!load_rom(&gi)) goto cleanup;
    if (!retro_load_game(&gi)) goto cleanup;

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    audio_init((unsigned)av.timing.sample_rate, g_settings.audio_buffer_ms);

    ensure_save_dir();
    sram_load();
    // Auto-resume: continue from the freshest auto-state (a full snapshot, so
    // it supersedes the SRAM load above). auto1 is refreshed every minute and
    // on exit, so it's the most up-to-date restore point.
    state_load("auto1");

    bool quit = false;
    unsigned frame = 0;
    while (appletMainLoop() && !quit) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (g_menu_open) {
            menu_input(down, &quit);
        } else if (down & HidNpadButton_ZR) {
            g_menu_open = true;         // open menu; game pauses (no retro_run)
            g_menu_level = 0;
            g_menu_sel = 0;
        } else {
            g_held = padGetButtons(&pad);
            u64 run_t0 = armGetSystemTick();
            retro_run();                // advances the game, fills g_frame
            run_timing_tick((unsigned)(armTicksToNs(armGetSystemTick() - run_t0) / 1000));
            fps_tick();                 // only counts real gameplay frames
            frame++;
            if (frame % 600 == 0)    sram_save();          // SRAM ~every 10s
            if (frame % 3600 == 0)   state_save("auto1");  // auto ~every 1 min
            if (frame % 36000 == 0)  state_save("auto10"); // auto ~every 10 min
        }
        present();
    }

    sram_save();            // final flush on exit
    state_save("auto1");    // snapshot on exit so next launch resumes here
    save_io_flush();        // these two must actually land before we exit
    retro_unload_game();
cleanup:
    if (g_net_initialized) net_upscale_exit();
    free(g_luma_buf);
    save_io_exit();
    audio_exit();
    retro_deinit();
    free(g_rom);
    free(g_map_x);
    free(g_map_y);
    free(g_native_frame);
    if (g_use_gpu) {
        gpu_video_exit();
        free(g_menu_frame);
        free(g_hud_frame);
    } else {
        free(g_frame);
        framebufferClose(&g_fb);
    }
    romfsExit();
    return 0;
}
