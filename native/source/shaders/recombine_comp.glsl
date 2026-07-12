#version 460
// Network-upscale recombine: takes the 3x-upscaled luma plane received from
// the server (packed bytes in an SSBO) plus the original game frame (for
// chroma), and writes the recombined RGB into the same output image the
// local ESPCN path uses. This is espcn3_comp.glsl's tail with the network's
// luma substituted for the conv output — moving the recombine off the CPU,
// where its integer version still cost ~2-4ms of the serial frame budget
// (and its original float version once capped the whole game at ~6fps).
//
// One invocation per SOURCE pixel, writing its 3x3 output block — chroma is
// sampled once per source pixel, mirroring both the espcn3 shader and the
// CPU implementation this replaces.
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D srcImage;                    // original RGB, for chroma
layout(binding = 0, rgba8) uniform writeonly image2D outImage;     // 3x-upscaled RGB result

layout(std140, binding = 0) uniform Dims { uvec2 size; } dims;     // native (pre-upscale) W,H

layout(std430, binding = 0) readonly buffer Luma { uint luma[]; }; // upscaled luma, 4 bytes/word

const int R = 3;

float luma_at(uint idx) {
    uint word = luma[idx >> 2];
    return float((word >> ((idx & 3u) * 8u)) & 0xFFu) / 255.0;
}

vec3 ycbcr_to_rgb(float y, float cb, float cr) {
    float r = y + 1.402 * (cr - 0.5);
    float g = y - 0.344136 * (cb - 0.5) - 0.714136 * (cr - 0.5);
    float b = y + 1.772 * (cb - 0.5);
    return clamp(vec3(r, g, b), 0.0, 1.0);
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = ivec2(dims.size);
    if (p.x >= sz.x || p.y >= sz.y) return;

    vec3 srcColor = texelFetch(srcImage, p, 0).rgb;
    float cb = -0.168736 * srcColor.r - 0.331264 * srcColor.g + 0.5 * srcColor.b + 0.5;
    float cr = 0.5 * srcColor.r - 0.418688 * srcColor.g - 0.081312 * srcColor.b + 0.5;

    uint outW = uint(sz.x) * uint(R);
    for (int ry = 0; ry < R; ry++) {
        for (int rx = 0; rx < R; rx++) {
            ivec2 op = p * R + ivec2(rx, ry);
            float yNew = luma_at(uint(op.y) * outW + uint(op.x));
            imageStore(outImage, op, vec4(ycbcr_to_rgb(yNew, cb, cr), 1.0));
        }
    }
}
