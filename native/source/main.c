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
#include "libretro.h"
#include "osd.h"

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
// 2 = load-slot list.
#define SLOT_COUNT 10            // manual slots 0..9
#define LOAD_COUNT (SLOT_COUNT + 2)  // + auto1 + auto10
static bool g_menu_open = false;
static int g_menu_level = 0;
static int g_menu_sel = 0;

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

    const int bpp = (g_px_fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
    for (unsigned dy = 0; dy < DST_H; dy++) {
        const u8 *src_row = (const u8 *)data + (size_t)g_map_y[dy] * pitch;
        u32 *dst_row = g_frame + (size_t)(DST_Y0 + dy) * FB_W + DST_X0;
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

// Serialize the whole machine to <game>.<ext> (atomically via a temp file).
static void state_save(const char *ext) {
    size_t size = retro_serialize_size();
    if (!size) return;
    void *buf = malloc(size);
    if (!buf) return;
    if (retro_serialize(buf, size)) {
        char path[256], tmp[264];
        save_path(path, sizeof(path), ext);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            fwrite(buf, 1, size, f);
            fclose(f);
            remove(path);
            rename(tmp, path);
        }
    }
    free(buf);
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

// --- in-game menu ------------------------------------------------------------
static void draw_menu(u32 *fb, u32 stride) {
    const int scale = 3;
    const int lh = 8 * scale + 12;
    int count = g_menu_level == 0 ? 4 : g_menu_level == 1 ? SLOT_COUNT : LOAD_COUNT;
    const char *title = g_menu_level == 0 ? "MENU"
                        : g_menu_level == 1 ? "SAVE STATE" : "LOAD STATE";

    osd_rect(fb, stride, 0, 0, FB_W, FB_H, 0xB0000000u);  // dim the game
    int pw = 620, ph = lh * (count + 2) + 40;
    int px = (FB_W - pw) / 2, py = (FB_H - ph) / 2;
    osd_rect(fb, stride, px, py, pw, ph, 0xF0181818u);    // panel
    osd_rect(fb, stride, px, py, pw, 4, 0xFF66CCFFu);     // accent bar

    int tx = px + 40, ty = py + 28;
    osd_text(fb, stride, tx, ty, 0xFF66CCFFu, scale, title);
    ty += lh + 10;

    for (int i = 0; i < count; i++) {
        char line[48];
        if (g_menu_level == 0) {
            const char *m[] = {"Resume", "Save", "Load", "Exit"};
            snprintf(line, sizeof(line), "%s", m[i]);
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
        if (i == g_menu_sel) osd_text(fb, stride, px + 14, ty, col, scale, ">");
        osd_text(fb, stride, tx, ty, col, scale, line);
        ty += lh;
    }
    osd_text(fb, stride, tx, py + ph - 26, 0xFF888888u, 2,
             "UP/DOWN move   A select   B back   ZR close");
}

// Present the last game frame (+ menu overlay if open) to the display.
static void present(void) {
    u32 stride;
    u32 *out = (u32 *)framebufferBegin(&g_fb, &stride);
    const u32 row_px = stride / sizeof(u32);
    for (int y = 0; y < FB_H; y++)
        memcpy(out + (size_t)y * row_px, g_frame + (size_t)y * FB_W,
               FB_W * sizeof(u32));
    if (g_menu_open) draw_menu(out, row_px);
    framebufferEnd(&g_fb);
}

// Handle a frame of input while the menu is open.
static void menu_input(u64 down, bool *quit) {
    int count = g_menu_level == 0 ? 4 : g_menu_level == 1 ? SLOT_COUNT : LOAD_COUNT;
    if (down & (HidNpadButton_Down | HidNpadButton_StickLDown))
        g_menu_sel = (g_menu_sel + 1) % count;
    if (down & (HidNpadButton_Up | HidNpadButton_StickLUp))
        g_menu_sel = (g_menu_sel + count - 1) % count;
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
            else *quit = true;                                             // Exit
        } else if (g_menu_level == 1) {
            char ext[16];
            snprintf(ext, sizeof(ext), "slot%d", g_menu_sel);
            state_save(ext);
            g_menu_open = false;
        } else {
            char ext[16];
            load_ext(g_menu_sel, ext, sizeof(ext));
            state_load(ext);
            g_menu_open = false;
        }
    }
}

int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    romfsInit();

    NWindow *win = nwindowGetDefault();
    framebufferCreate(&g_fb, win, FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&g_fb);

    // Persistent frame (opaque black to start, incl. the side pillar-bars).
    g_frame = malloc((size_t)FB_W * FB_H * sizeof(u32));
    for (int i = 0; i < FB_W * FB_H; i++) g_frame[i] = 0xFF000000u;

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
            retro_run();                // advances the game, fills g_frame
            frame++;
            if (frame % 600 == 0)    sram_save();          // SRAM ~every 10s
            if (frame % 3600 == 0)   state_save("auto1");  // auto ~every 1 min
            if (frame % 36000 == 0)  state_save("auto10"); // auto ~every 10 min
        }
        present();
    }

    sram_save();            // final flush on exit
    state_save("auto1");    // snapshot on exit so next launch resumes here
    retro_unload_game();
cleanup:
    audio_exit();
    retro_deinit();
    free(g_rom);
    free(g_frame);
    free(g_map_x);
    free(g_map_y);
    framebufferClose(&g_fb);
    romfsExit();
    return 0;
}
