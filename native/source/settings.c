#include "settings.h"

#include <stdio.h>
#include <sys/stat.h>

#define SETTINGS_DIR  "sdmc:/switch/ebbswitchport"
#define SETTINGS_PATH SETTINGS_DIR "/settings.cfg"
#define SETTINGS_TMP  SETTINGS_PATH ".tmp"

void settings_load(Settings *out) {
    // Off by default: the deko3d GPU path is a first, unproven cut (crashed on
    // real hardware during initial testing) — opt-in from the Settings menu
    // until it's verified solid, rather than every install hitting it blind.
    out->hw_accel = false;
    out->audio_buffer_ms = 30;
    out->show_hud = false;
    out->dynamic_overclock = true;
    out->overclock_trigger_dupes = 5;
    out->overclock_boost_frames = 100;
    out->crt_mode = false;
    out->ai_upscale = false;  // never persisted (see settings_save) — always requires
                               // an explicit opt-in each session, regardless of what
                               // was last saved; see the "N/A"/crash-risk notes on
                               // gpu_video_set_ai_upscale.

    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "hw_accel=%d", &v) == 1) out->hw_accel = v != 0;
        else if (sscanf(line, "audio_buffer_ms=%d", &v) == 1) out->audio_buffer_ms = (unsigned)v;
        else if (sscanf(line, "show_hud=%d", &v) == 1) out->show_hud = v != 0;
        else if (sscanf(line, "dynamic_overclock=%d", &v) == 1) out->dynamic_overclock = v != 0;
        else if (sscanf(line, "overclock_trigger_dupes=%d", &v) == 1) out->overclock_trigger_dupes = (unsigned)v;
        else if (sscanf(line, "overclock_boost_frames=%d", &v) == 1) out->overclock_boost_frames = (unsigned)v;
        else if (sscanf(line, "crt_mode=%d", &v) == 1) out->crt_mode = v != 0;
        // ai_upscale intentionally not read back — always starts false.
    }
    fclose(f);
}

void settings_save(const Settings *s) {
    mkdir("sdmc:/switch", 0777);
    mkdir(SETTINGS_DIR, 0777);
    FILE *f = fopen(SETTINGS_TMP, "wb");
    if (!f) return;
    // ai_upscale intentionally not written — never persisted, see settings_load.
    fprintf(f, "hw_accel=%d\naudio_buffer_ms=%u\nshow_hud=%d\ndynamic_overclock=%d\n"
               "overclock_trigger_dupes=%u\noverclock_boost_frames=%u\ncrt_mode=%d\n",
            s->hw_accel ? 1 : 0, s->audio_buffer_ms, s->show_hud ? 1 : 0,
            s->dynamic_overclock ? 1 : 0, s->overclock_trigger_dupes, s->overclock_boost_frames,
            s->crt_mode ? 1 : 0);
    fclose(f);
    remove(SETTINGS_PATH);
    rename(SETTINGS_TMP, SETTINGS_PATH);
}
