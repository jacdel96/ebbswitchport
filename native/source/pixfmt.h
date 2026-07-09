// Pixel-format conversion shared by both render backends: the CPU path
// (main.c's fused convert+upscale loop) and the GPU path (gpu_video.c, which
// only needs convert — the GPU does the upscale by sampling the result).
#pragma once
#include <stdlib.h>
#include <switch.h>

#include "libretro.h"

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

// 16-bit source pixel -> RGBA8 lookup table. RETRO_PIXEL_FORMAT_RGB565 and
// RETRO_PIXEL_FORMAT_0RGB1555 both fit this shape closely enough for opaque
// display (bit 15 of 0RGB1555 is always 0, same top bit rgb565_to_rgba treats
// as the low bit of a 5-bit green field — visually indistinguishable at 8bpc).
typedef struct {
    u32 *table;               // 65536 entries, lazily allocated
    enum retro_pixel_format fmt;
} PixelLut;

static inline void pixlut_rebuild(PixelLut *lut, enum retro_pixel_format fmt) {
    if (!lut->table) lut->table = malloc(65536 * sizeof(u32));
    for (u32 p = 0; p < 65536; p++) lut->table[p] = rgb565_to_rgba((u16)p);
    lut->fmt = fmt;
}

// Converts one core frame into a tightly-packed RGBA8 buffer at the SAME
// resolution as the source (no scaling) — the GPU path's upload source.
static inline void pixfmt_convert_to_rgba8(u32 *dst, const void *data, unsigned width,
                                            unsigned height, size_t pitch,
                                            enum retro_pixel_format fmt, PixelLut *lut) {
    const int bpp = (fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
    if (bpp == 2 && lut->fmt != fmt) pixlut_rebuild(lut, fmt);
    for (unsigned y = 0; y < height; y++) {
        const u8 *src_row = (const u8 *)data + (size_t)y * pitch;
        u32 *dst_row = dst + (size_t)y * width;
        if (bpp == 2) {
            const u16 *s = (const u16 *)src_row;
            for (unsigned x = 0; x < width; x++) dst_row[x] = lut->table[s[x]];
        } else {
            const u32 *s = (const u32 *)src_row;
            for (unsigned x = 0; x < width; x++) dst_row[x] = xrgb8888_to_rgba(s[x]);
        }
    }
}

// Generic nearest-neighbor RGBA8->RGBA8 upscale into a sub-rect of a larger
// destination buffer. Used only off the hot path (building the paused-menu
// backdrop for the GPU overlay), so it's not fused/optimized like main.c's
// per-pixel-format CPU scaler.
static inline void nn_upscale_rgba(u32 *dst, unsigned dst_stride, int dst_x0, int dst_y0,
                                    unsigned dst_w, unsigned dst_h,
                                    const u32 *src, unsigned src_w, unsigned src_h) {
    for (unsigned dy = 0; dy < dst_h; dy++) {
        unsigned sy = dy * src_h / dst_h;
        u32 *drow = dst + (size_t)(dst_y0 + dy) * dst_stride + dst_x0;
        const u32 *srow = src + (size_t)sy * src_w;
        for (unsigned dx = 0; dx < dst_w; dx++)
            drow[dx] = srow[dx * src_w / dst_w];
    }
}
