// ebbswitchport native frontend — Milestone 2: snes9x libretro core + video.
//
// Statically links the snes9x libretro core, loads romfs:/game.sfc, and drives
// a libnx framebuffer from the core's video callback. Input is mapped from the
// Switch pad to the SNES layout. Audio and persistence are stubbed here and
// filled in by M3/M4.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "audio.h"
#include "libretro.h"

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

// --- pixel conversion --------------------------------------------------------
static inline u32 rgb565_to_rgba(u16 p) {
    u32 r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline u32 xrgb8888_to_rgba(u32 p) {
    u32 r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// --- libretro callbacks ------------------------------------------------------
static void video_refresh(const void *data, unsigned width, unsigned height,
                          size_t pitch) {
    if (!data) return;  // duped frame

    // Rebuild scale maps if the core's output geometry changed.
    if (width != g_map_w || height != g_map_h) {
        free(g_map_x); free(g_map_y);
        g_map_x = malloc(sizeof(int) * DST_W);
        g_map_y = malloc(sizeof(int) * DST_H);
        for (unsigned dx = 0; dx < DST_W; dx++) g_map_x[dx] = dx * width / DST_W;
        for (unsigned dy = 0; dy < DST_H; dy++) g_map_y[dy] = dy * height / DST_H;
        g_map_w = width; g_map_h = height;
    }

    u32 stride;
    u32 *out = (u32 *)framebufferBegin(&g_fb, &stride);
    const u32 row_px = stride / sizeof(u32);
    const int bpp = (g_px_fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;

    for (unsigned dy = 0; dy < DST_H; dy++) {
        const u8 *src_row = (const u8 *)data + (size_t)g_map_y[dy] * pitch;
        u32 *dst_row = out + (size_t)(DST_Y0 + dy) * row_px + DST_X0;
        if (bpp == 2) {
            const u16 *s = (const u16 *)src_row;
            for (unsigned dx = 0; dx < DST_W; dx++)
                dst_row[dx] = rgb565_to_rgba(s[g_map_x[dx]]);
        } else {
            const u32 *s = (const u32 *)src_row;
            for (unsigned dx = 0; dx < DST_W; dx++)
                dst_row[dx] = xrgb8888_to_rgba(s[g_map_x[dx]]);
        }
    }
    framebufferEnd(&g_fb);
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
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        return false;  // core uses its defaults
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

// Flush the core's live SRAM to disk (atomically via a temp file).
static void sram_save(void) {
    void *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size) return;
    char path[256], tmp[264];
    save_path(path, sizeof(path), "srm");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(mem, 1, size, f);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

static void state_save(void) {
    size_t size = retro_serialize_size();
    if (!size) return;
    void *buf = malloc(size);
    if (!buf) return;
    if (retro_serialize(buf, size)) {
        char path[256];
        save_path(path, sizeof(path), "state");
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(buf, 1, size, f); fclose(f); }
    }
    free(buf);
}

static void state_load(void) {
    char path[256];
    save_path(path, sizeof(path), "state");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(size);
    if (buf && fread(buf, 1, size, f) == (size_t)size)
        retro_unserialize(buf, size);
    free(buf);
    fclose(f);
}

int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    romfsInit();

    NWindow *win = nwindowGetDefault();
    framebufferCreate(&g_fb, win, FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&g_fb);

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
    audio_init((unsigned)av.timing.sample_rate);

    ensure_save_dir();
    sram_load();

    const u64 EXIT_COMBO = HidNpadButton_L | HidNpadButton_R |
                           HidNpadButton_Plus | HidNpadButton_Minus;
    unsigned frame = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        g_held = padGetButtons(&pad);
        u64 down = padGetButtonsDown(&pad);

        if ((g_held & EXIT_COMBO) == EXIT_COMBO)
            break;
        // Save states on the triggers (ZR = save, ZL = load) — not SNES buttons.
        if (down & HidNpadButton_ZR) state_save();
        if (down & HidNpadButton_ZL) state_load();

        retro_run();  // drives video_refresh -> framebuffer blit

        if (++frame % 600 == 0)  // flush SRAM to disk ~every 10s
            sram_save();
    }

    sram_save();          // final flush on exit
    retro_unload_game();
cleanup:
    audio_exit();
    retro_deinit();
    free(g_rom);
    free(g_map_x);
    free(g_map_y);
    framebufferClose(&g_fb);
    romfsExit();
    return 0;
}
