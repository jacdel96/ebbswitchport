#version 460
// ESPCN layer 3: 3x3 conv, 32 -> 9 channels (r=3, no activation), then the
// pixel-shuffle rearrangement into a 3x-upscaled luma image (free — pure
// data rearrangement, see the memo). The 9 channels map to a 3x3 output
// block per input pixel as channel index c = ry*3 + rx (validated against
// torch.nn.PixelShuffle's actual behavior, not just its docs — see
// validate.py's pixel_shuffle()).
//
// Only luma went through the network (ESPCN's own convention, not a
// shortcut taken here). Chroma is reconstructed via a standard YCbCr
// round-trip: sample the original color at the source pixel, derive Cb/Cr,
// recombine with the network's new Y for each of the 9 output sub-pixels.
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D srcImage;                    // original RGB, for chroma
layout(binding = 0, rgba8) uniform writeonly image2D outImage;     // 3x-upscaled RGB result

layout(std140, binding = 0) uniform Dims { uvec2 size; } dims;     // native (pre-upscale) W,H

layout(std430, binding = 0) readonly buffer Weights { float w[]; };
layout(std430, binding = 1) readonly buffer Feat2 { float feat2[]; };

const int W3_OFF = 20128;   // (9,32,3,3) = 2592 floats
const int B3_OFF = 22720;   // 9 floats
const int R = 3;

float feat2_at(int ic, ivec2 p, ivec2 sz) {
    if (p.x < 0 || p.y < 0 || p.x >= sz.x || p.y >= sz.y) return 0.0;
    return feat2[ic * sz.y * sz.x + p.y * sz.x + p.x];
}

vec3 ycbcr_to_rgb(float y, float cb, float cr) {
    float r = y + 1.402 * (cr - 0.5);
    float g = y - 0.344136 * (cb - 0.5) - 0.714136 * (cr - 0.5);
    float b = y + 1.772 * (cb - 0.5);
    return clamp(vec3(r, g, b), 0.0, 1.0);
}

// Workgroup-shared tile: an 8x8 workgroup's 3x3-conv footprint spans a 10x10
// halo region, all 32 feat2 channels — cooperatively loaded ONCE per
// workgroup instead of every thread independently re-fetching its own
// neighborhood from the (7.3MB) feat2 buffer.
const int HALO = 1;
const int TILE = 8 + 2 * HALO;  // 10
const int IC = 32;
const int TILE2 = TILE * TILE;
shared float sharedFeat2[IC * TILE2];  // flat, 32*10*10*4B = 12.8KB — see
                                        // espcn2_comp.glsl for why flat, not 3D.

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = ivec2(dims.size);
    ivec2 localId = ivec2(gl_LocalInvocationID.xy);
    ivec2 tileBase = p - localId;

    int li = int(gl_LocalInvocationIndex);
    int total = IC * TILE2;
    for (int idx = li; idx < total; idx += 64) {
        int ic = idx / TILE2;
        int rem = idx % TILE2;
        int ty = rem / TILE, tx = rem % TILE;
        sharedFeat2[idx] = feat2_at(ic, tileBase + ivec2(tx - HALO, ty - HALO), sz);
    }
    barrier();

    if (p.x >= sz.x || p.y >= sz.y) return;

    // Each (ic, ky, kx) feat2 value is shared by all 9 output channels — loop
    // it on the outside and accumulate into all 9 channels per read, instead
    // of re-reading it once per channel (9x fewer feat2 reads on top of the
    // cross-thread sharing above).
    float outc[9];
    for (int oc = 0; oc < 9; oc++) outc[oc] = w[B3_OFF + oc];

    for (int ic = 0; ic < 32; ic++) {
        for (int ky = -1; ky <= 1; ky++) {
            for (int kx = -1; kx <= 1; kx++) {
                int sidx = ic * TILE2 + (localId.y + ky + HALO) * TILE + (localId.x + kx + HALO);
                float v = sharedFeat2[sidx];
                int kidx = (ky + 1) * 3 + (kx + 1);
                for (int oc = 0; oc < 9; oc++) {
                    int widx = W3_OFF + oc * 32 * 9 + ic * 9 + kidx;
                    outc[oc] += w[widx] * v;
                }
            }
        }
    }
    // no activation on the final layer

    vec3 srcColor = texelFetch(srcImage, p, 0).rgb;
    float cb = -0.168736 * srcColor.r - 0.331264 * srcColor.g + 0.5 * srcColor.b + 0.5;
    float cr = 0.5 * srcColor.r - 0.418688 * srcColor.g - 0.081312 * srcColor.b + 0.5;

    for (int ry = 0; ry < R; ry++) {
        for (int rx = 0; rx < R; rx++) {
            float yNew = outc[ry * R + rx];
            vec3 rgb = ycbcr_to_rgb(yNew, cb, cr);
            imageStore(outImage, p * R + ivec2(rx, ry), vec4(rgb, 1.0));
        }
    }
}
