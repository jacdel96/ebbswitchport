// GPU-accelerated rendering backend (deko3d), selected once at startup as an
// alternative to the CPU Framebuffer path in main.c (see settings.h — this is
// a next-launch-only choice, never swapped live). The CPU still converts the
// core's pixel format to RGBA8 (see pixfmt.h); this module only ever deals in
// tightly-packed RGBA8 source buffers and does the *scaling* on the GPU by
// nearest-sampling them onto quads.
//
// gpu_video_init returns false on ANY failure (device/swapchain/shader/memory)
// after unwinding whatever it had allocated so far — callers must fall back to
// the CPU path rather than treating false as fatal, since this backend has
// never run on real hardware.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <switch.h>

// The small HUD panel's fixed pixel size (shared with main.c, which builds the
// panel's pixels via osd_rect/osd_text before handing them to gpu_video_set_hud).
// Wide enough for "FPS 60.0  AUDIO 100ms  LAG 100%" at scale 2 plus padding.
#define HUD_W 560
#define HUD_H 56

bool gpu_video_init(NWindow *win);

// Copies `w`x`h` tightly-packed RGBA8 pixels (the core's native resolution) as
// the next game frame to display, nearest-sampled up to the centered 960x720
// game window on presentation. w/h must not exceed 512x512 (SNES's worst case
// with headroom); out-of-range calls are ignored (last good frame persists).
void gpu_video_upload_frame(const uint32_t *rgba8, unsigned w, unsigned h);

// Sets/clears the small live HUD panel (exactly HUD_W x HUD_H RGBA8, opaque);
// NULL hides it. Cheap enough to call every gameplay frame.
void gpu_video_set_hud(const uint32_t *rgba8, unsigned w, unsigned h);

// Sets/clears the full 1280x720 opaque paused-menu backdrop (replaces the game
// window + HUD entirely while visible); NULL hides it.
void gpu_video_set_overlay(const uint32_t *rgba_1280x720);

// Uploads whatever changed since the last call, draws, and presents. Call once
// per main-loop iteration, whether or not a new game frame arrived that tick
// (mirrors the CPU path's present(), which redraws the last frame every tick).
void gpu_video_present(void);

void gpu_video_exit(void);
