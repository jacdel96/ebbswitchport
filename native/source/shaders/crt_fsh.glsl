#version 460

// CRT look for the game quad only (HUD/menu overlay keep the plain shader —
// see gpu_video.c's shader bind around the game draw). Single pass, no
// curvature/bloom for this first cut — those need UV warping and a second
// blur pass respectively, more tuning risk for a first look. Everything here
// is screen-space (gl_FragCoord), not tied to the source texture's native
// resolution, so the scanline/mask pitch stays a fixed physical size.
layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D texture0;

void main() {
    vec4 c = texture(texture0, inUV);

    // Dark line every ~3 physical output rows — roughly one line per native
    // SNES scanline at our ~3.2x upscale (960x720 from ~256x224). Alternating
    // every single physical pixel row instead (mod 2) produced lines too fine
    // to read as scanlines at normal viewing distance — this groups them into
    // visible bands instead, at a much stronger darkening.
    float row = mod(gl_FragCoord.y, 3.0);
    float scan = row < 1.0 ? 0.45 : 1.0;

    // Strong RGB subpixel striping across columns.
    float col = mod(gl_FragCoord.x, 3.0);
    vec3 mask = col < 1.0 ? vec3(1.35, 0.7, 0.7)
              : col < 2.0 ? vec3(0.7, 1.35, 0.7)
                          : vec3(0.7, 0.7, 1.35);

    // Stronger darkening toward the edges of the game image.
    vec2 d = inUV - 0.5;
    float vig = 1.0 - dot(d, d) * 0.7;

    outColor = vec4(c.rgb * scan * mask * vig, c.a);
}
