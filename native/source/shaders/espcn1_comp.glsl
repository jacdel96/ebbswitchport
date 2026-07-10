#version 460
// ESPCN layer 1: 5x5 conv, luma (extracted from the source RGB texture) -> 64
// channels, tanh. See gpu_video.c's run_espcn_upscale for the full pipeline
// and native/tools/package_espcn_weights.py for how the weight blob below is
// laid out (this shader's offsets must match that script's WEIGHT_LAYOUT).
//
// Zero-padding at the edges — matches PyTorch's Conv2d default exactly (the
// weights were validated bit-for-bit against a from-scratch NumPy
// reimplementation using this same convention; edge-clamping would NOT match).
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D srcImage;

layout(std140, binding = 0) uniform Dims { uvec2 size; } dims;

layout(std430, binding = 0) readonly buffer Weights { float w[]; };
layout(std430, binding = 1) writeonly buffer Feat1 { float feat1[]; };

const int W1_OFF = 0;     // (64,1,5,5) = 1600 floats
const int B1_OFF = 1600;  // 64 floats

float luma_padded(ivec2 p, ivec2 sz) {
    if (p.x < 0 || p.y < 0 || p.x >= sz.x || p.y >= sz.y) return 0.0;
    vec3 c = texelFetch(srcImage, p, 0).rgb;
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = ivec2(dims.size);
    if (p.x >= sz.x || p.y >= sz.y) return;

    for (int oc = 0; oc < 64; oc++) {
        float acc = w[B1_OFF + oc];
        for (int ky = -2; ky <= 2; ky++) {
            for (int kx = -2; kx <= 2; kx++) {
                float v = luma_padded(p + ivec2(kx, ky), sz);
                int widx = W1_OFF + oc * 25 + (ky + 2) * 5 + (kx + 2);
                acc += w[widx] * v;
            }
        }
        feat1[oc * sz.y * sz.x + p.y * sz.x + p.x] = tanh(acc);
    }
}
