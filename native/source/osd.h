// Tiny on-screen-display primitives: draw filled (optionally translucent)
// rectangles and 8x8 bitmap text directly into our RGBA8888 framebuffer.
// Used by the in-game menu. Colors are 0xAABBGGRR (same layout our pixel
// converters produce): R = low byte ... A = high byte.
#pragma once
#include <switch.h>

// Fill w*h at (x,y), clipped to the stride_px x canvas_h canvas. If the
// color's alpha < 255 it is alpha-blended over the existing framebuffer
// contents (for the dim-the-game overlay).
void osd_rect(u32 *fb, u32 stride_px, u32 canvas_h, int x, int y, int w, int h,
              u32 rgba);

// Draw a NUL-terminated ASCII string; each glyph is 8x8 scaled by `scale`.
void osd_text(u32 *fb, u32 stride_px, u32 canvas_h, int x, int y, u32 rgba,
              int scale, const char *s);

// Pixel width a string would occupy at the given scale.
int osd_text_w(const char *s, int scale);
