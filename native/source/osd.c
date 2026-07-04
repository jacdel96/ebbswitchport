#include "osd.h"

#include <string.h>

#include "font8x8_basic.h"  // char font8x8_basic[128][8]; included only here

#define FB_W 1280
#define FB_H 720

static inline u32 blend(u32 dst, u32 src, u32 a) {
    // channels are byte-packed 0xAABBGGRR; blend low 3 bytes, keep opaque out.
    u32 dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF;
    u32 sr = src & 0xFF, sg = (src >> 8) & 0xFF, sb = (src >> 16) & 0xFF;
    u32 r = (sr * a + dr * (255 - a)) / 255;
    u32 g = (sg * a + dg * (255 - a)) / 255;
    u32 b = (sb * a + db * (255 - a)) / 255;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void osd_rect(u32 *fb, u32 stride_px, int x, int y, int w, int h, u32 rgba) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;

    u32 a = (rgba >> 24) & 0xFF;
    for (int yy = 0; yy < h; yy++) {
        u32 *row = fb + (size_t)(y + yy) * stride_px + x;
        if (a == 0xFF)
            for (int xx = 0; xx < w; xx++) row[xx] = rgba;
        else
            for (int xx = 0; xx < w; xx++) row[xx] = blend(row[xx], rgba, a);
    }
}

void osd_text(u32 *fb, u32 stride_px, int x, int y, u32 rgba, int scale,
              const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        const char *glyph = font8x8_basic[c];
        for (int row = 0; row < 8; row++) {
            u8 bits = (u8)glyph[row];
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1 << col))) continue;  // font is LSB=leftmost
                osd_rect(fb, stride_px, x + col * scale, y + row * scale,
                         scale, scale, rgba);
            }
        }
        x += 8 * scale;
    }
}

int osd_text_w(const char *s, int scale) {
    return (int)strlen(s) * 8 * scale;
}
