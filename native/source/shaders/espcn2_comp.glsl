#version 460
// ESPCN layer 2: 3x3 conv, 64 -> 32 channels, tanh. See espcn1_comp.glsl for
// the padding-convention note (applies identically here).
layout(local_size_x = 8, local_size_y = 8) in;

layout(std140, binding = 0) uniform Dims { uvec2 size; } dims;

layout(std430, binding = 0) readonly buffer Weights { float w[]; };
layout(std430, binding = 1) readonly buffer Feat1 { float feat1[]; };
layout(std430, binding = 2) writeonly buffer Feat2 { float feat2[]; };

const int W2_OFF = 1664;    // (32,64,3,3) = 18432 floats
const int B2_OFF = 20096;   // 32 floats

float feat1_at(int ic, ivec2 p, ivec2 sz) {
    if (p.x < 0 || p.y < 0 || p.x >= sz.x || p.y >= sz.y) return 0.0;
    return feat1[ic * sz.y * sz.x + p.y * sz.x + p.x];
}

// Workgroup-shared tile: an 8x8 workgroup's 3x3-conv footprint spans a 10x10
// halo region, all 64 feat1 channels — cooperatively loaded ONCE per
// workgroup (6400 reads total) instead of every thread independently
// re-fetching its own neighborhood from the (14.7MB, nowhere near
// cache-resident) feat1 buffer.
const int HALO = 1;
const int TILE = 8 + 2 * HALO;  // 10
const int IC = 64;
const int TILE2 = TILE * TILE;
shared float sharedFeat1[IC * TILE2];  // flat, 64*10*10*4B = 25.6KB — a 3D
                                        // array here crashed uam (bus error),
                                        // flat + manual indexing does not.

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
        sharedFeat1[idx] = feat1_at(ic, tileBase + ivec2(tx - HALO, ty - HALO), sz);
    }
    barrier();

    if (p.x >= sz.x || p.y >= sz.y) return;

    // Each (ic, ky, kx) feat1 value is shared by all 32 output channels — loop
    // it on the outside and accumulate into all 32 channels per read, instead
    // of re-reading it once per channel (32x fewer feat1 reads on top of the
    // cross-thread sharing above).
    float acc[32];
    for (int oc = 0; oc < 32; oc++) acc[oc] = w[B2_OFF + oc];

    for (int ic = 0; ic < 64; ic++) {
        for (int ky = -1; ky <= 1; ky++) {
            for (int kx = -1; kx <= 1; kx++) {
                int sidx = ic * TILE2 + (localId.y + ky + HALO) * TILE + (localId.x + kx + HALO);
                float v = sharedFeat1[sidx];
                int kidx = (ky + 1) * 3 + (kx + 1);
                for (int oc = 0; oc < 32; oc++) {
                    int widx = W2_OFF + oc * 64 * 9 + ic * 9 + kidx;
                    acc[oc] += w[widx] * v;
                }
            }
        }
    }
    for (int oc = 0; oc < 32; oc++)
        feat2[oc * sz.y * sz.x + p.y * sz.x + p.x] = tanh(acc[oc]);
}
