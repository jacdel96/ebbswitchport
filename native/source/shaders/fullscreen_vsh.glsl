#version 460

// Emits a full clip-space quad from a bare 4-vertex triangle strip draw call
// (no vertex/index buffer) — the same trick used to size/position it differs
// per draw only via the bound viewport/scissor (see gpu_video.c).
const vec2 positions[4] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
// Confirmed on hardware: v=0 maps to the BOTTOM of the screen under deko3d's
// default convention, opposite our row-major RGBA8 buffers (row 0 = top, same
// indexing the CPU Framebuffer path uses) — flipped from the initial guess.
const vec2 texcoords[4] = vec2[](vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0));

layout (location = 0) out vec2 outUV;

void main() {
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    outUV = texcoords[gl_VertexID];
}
