#include "gpu_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deko3d.h>

// --- layout constants ---------------------------------------------------------
// Unlike the CPU Framebuffer path (which stays fixed at 720p always — see
// settings.h), the GPU swapchain tracks dock/handheld live: 720p handheld,
// 1080p docked. g_fb_w/h is the swapchain's CURRENT resolution; the game
// quad's position/size (g_dst_*) is recomputed to match on every resize.
// The overlay/menu-backdrop texture stays fixed at OVERLAY_W x OVERLAY_H
// regardless — main.c always builds it at that size, and it's just sampled
// by a bigger or smaller output viewport, no different from any other scaled
// texture (see gpu_video_present's overlay draw).
#define FB_W_HANDHELD 1280
#define FB_H_HANDHELD 720
#define FB_W_DOCKED   1920
#define FB_H_DOCKED   1080
#define OVERLAY_W 1280
#define OVERLAY_H 720
static unsigned g_fb_w = FB_W_HANDHELD, g_fb_h = FB_H_HANDHELD;
static int g_dst_x0, g_dst_y0;
static unsigned g_dst_w, g_dst_h;

static void resolution_for_mode(unsigned *w, unsigned *h) {
    if (appletGetOperationMode() == AppletOperationMode_Console) { *w = FB_W_DOCKED; *h = FB_H_DOCKED; }
    else { *w = FB_W_HANDHELD; *h = FB_H_HANDHELD; }
}

// The game window keeps the same 4:3-within-16:9 framing at every resolution
// (960x720-in-1280x720 and 1440x1080-in-1920x1080 are the same proportions —
// docked is an exact 1.5x scale of handheld here — so one formula covers both).
static void compute_dst_layout(void) {
    g_dst_h = g_fb_h;
    g_dst_w = g_fb_h * 4 / 3;
    g_dst_x0 = (int)(g_fb_w - g_dst_w) / 2;
    g_dst_y0 = 0;
}

// Worst-case core output size (SNES is at most ~512x478 in hi-res/interlaced
// modes); the game texture is allocated once at this size and only its
// *logical* dimensions (see g_game_w/h) change if the core's geometry does.
#define GAME_MAX_W 512
#define GAME_MAX_H 512

#define N_FB       2   // swapchain depth
#define N_PARITY   2   // scratch double-buffering depth

// The three ESPCN compute shaders unroll deeply (32x64x9 / 9x32x9 nested
// loops), producing far larger compiled shader binaries than the tiny
// vertex/fragment shaders elsewhere in this file (hundreds of KB each,
// checked at build time) — 64KB was fine before, nowhere near enough now.
#define CODE_MEM_SIZE (2 * 1024 * 1024)
#define CMD_MEM_SIZE  (64 * 1024)

// Image descriptor slots (indices into the bound image descriptor set).
// IMG_ESPCN_OUT serves double duty: written via plain image load/store by
// espcn3_comp (dkMakeImageHandle) and later sampled as an ordinary texture
// for the game-quad draw (dkMakeTextureHandle) — same descriptor slot, two
// different handle constructors, per deko3d's shared image descriptor model.
enum { IMG_GAME = 0, IMG_HUD = 1, IMG_OVERLAY = 2, IMG_ESPCN_OUT = 3, IMG_COUNT = 4 };
#define SAMPLER_SLOT 0

// --- experimental AI upscale (ESPCN) — see the research memo -----------------
// Optional, best-effort: only enabled if a local (gitignored, unshipped —
// licensing is unresolved, see settings.h) weights file is present in romfs.
// Its absence is NOT a gpu_video_init failure; every other GPU feature works
// identically whether or not this is available.
#define ESPCN_SCALE 3
#define ESPCN_MAX_W 256   // standard SNES resolution + small headroom; hi-res
#define ESPCN_MAX_H 240   // core modes exceeding this just skip AI upscale that frame
#define ESPCN_WEIGHTS_PATH "romfs:/ai/espcn_x3.bin"
// Byte offsets into the weight blob (payload only — the 56-byte file header
// with magic/scale/dims is skipped on load), matching native's
// package_espcn_weights.py output exactly and mirrored in the espcn*_comp.glsl
// shaders' W1_OFF/B1_OFF/etc constants.
#define ESPCN_W1_COUNT (64 * 1 * 5 * 5)
#define ESPCN_B1_COUNT 64
#define ESPCN_W2_COUNT (32 * 64 * 3 * 3)
#define ESPCN_B2_COUNT 32
#define ESPCN_W3_COUNT (9 * 32 * 3 * 3)
#define ESPCN_B3_COUNT 9
#define ESPCN_WEIGHTS_FLOATS (ESPCN_W1_COUNT + ESPCN_B1_COUNT + ESPCN_W2_COUNT + \
                               ESPCN_B2_COUNT + ESPCN_W3_COUNT + ESPCN_B3_COUNT)

static inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// --- device-owned state --------------------------------------------------------
static bool g_ready = false;

static DkDevice g_device;
static DkQueue g_queue;

static NWindow *g_win;
static DkMemBlock g_fbMemBlock;
static DkImage g_fbImages[N_FB];
static DkImage g_fbImagesStaging[N_FB];  // resize_swapchain's scratch; static so it
                                          // never dangles regardless of whether
                                          // dkSwapchainCreate retains the pointers
static DkSwapchain g_swapchain;

static DkMemBlock g_codeMemBlock;
static uint32_t g_codeMemOffset;
static DkShader g_vsh, g_fsh, g_crtFsh;
static bool g_crt_enabled = false;

static DkMemBlock g_gameImgMem;
static DkImage g_gameImage;
static unsigned g_game_w = 0, g_game_h = 0;   // current logical size (0 = uninitialized)

static DkMemBlock g_hudImgMem;
static DkImage g_hudImage;
static bool g_hud_visible = false;

static DkMemBlock g_overlayImgMem;
static DkImage g_overlayImage;
static bool g_overlay_visible = false;

// CPU-visible upload staging, double-buffered per source (game/hud/overlay)
// so the CPU can write next frame's pixels while the GPU is still reading the
// previous frame's out of the other half.
static DkMemBlock g_gameScratch[N_PARITY];
static DkMemBlock g_hudScratch[N_PARITY];
static DkMemBlock g_overlayScratch[N_PARITY];
static unsigned g_parity = 0;

static DkMemBlock g_descMemBlock;   // 4 image descriptors + 1 sampler descriptor
#define DESC_IMG_OFFSET(slot) ((slot) * DK_IMAGE_DESCRIPTOR_ALIGNMENT)
#define DESC_SAMPLER_OFFSET   256

// --- ESPCN state ---------------------------------------------------------------
static bool g_espcn_available = false;  // weights present + all resources allocated OK
static bool g_espcn_enabled = false;    // user setting (gpu_video_set_ai_upscale)
static DkShader g_espcn1, g_espcn2, g_espcn3;
static DkMemBlock g_espcnWeights;       // SSBO: raw weight/bias blob (read-only)
static DkMemBlock g_espcnDimsUbo;       // UBO: current native W,H (as uvec2, std140-padded)
static DkMemBlock g_espcnFeat1Mem;      // SSBO: 64 x ESPCN_MAX_H x ESPCN_MAX_W floats
static DkMemBlock g_espcnFeat2Mem;      // SSBO: 32 x ESPCN_MAX_H x ESPCN_MAX_W floats
static DkMemBlock g_espcnOutImgMem;
static DkImage g_espcnOutImage;         // (ESPCN_MAX_W*3) x (ESPCN_MAX_H*3), load/store + sampled
static unsigned g_espcn_last_us = 0;    // wall-clock time of the last dispatch, for the HUD
static bool g_espcn_have_output = false; // g_espcnOutImage holds a valid result for the
                                          // current g_gameImage contents — see gpu_video_present's
                                          // dupe-frame skip (this is what makes reusing it correct
                                          // rather than stale garbage on the very first run).

// --- network AI-upscale state ---------------------------------------------------
// Alternative source for IMG_ESPCN_OUT (see gpu_video.h) — mutually exclusive
// with the local compute path at runtime, enforced by main.c only ever
// enabling one of the two settings at a time.
static bool g_network_enabled = false;
static bool g_network_have_output = false;
static bool g_network_pending = false;
static unsigned g_network_pending_w = 0, g_network_pending_h = 0;
static DkMemBlock g_networkScratch[N_PARITY];  // CPU-visible RGBA8 recombine target

static DkMemBlock g_cmdbufMemBlock;
static DkCmdBuf g_cmdbuf;

// Per-frame "did upload_frame/set_hud/set_overlay write new pixels since the
// last present?" flags — drives which copyBufferToImage calls get recorded.
static bool g_game_pending = false, g_hud_pending = false, g_overlay_pending = false;
static unsigned g_pending_game_w = 0, g_pending_game_h = 0;

// --- init helpers --------------------------------------------------------------

// Loads a shader from romfs into the code memblock. Returns false (without
// touching *out) if the file is missing or the resulting shader is invalid —
// both are treated as ordinary init failures, not crashes.
static bool load_shader(DkShader *out, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }

    uint32_t offset = align_up(g_codeMemOffset, DK_SHADER_CODE_ALIGNMENT);
    if (offset + (uint32_t)size > CODE_MEM_SIZE) { fclose(f); return false; }
    void *dst = (uint8_t *)dkMemBlockGetCpuAddr(g_codeMemBlock) + offset;
    bool ok = fread(dst, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) return false;
    g_codeMemOffset = offset + (uint32_t)size;

    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, g_codeMemBlock, offset);
    dkShaderInitialize(out, &maker);
    return dkShaderIsValid(out);
}

// Writes (or rewrites, on a geometry change) the image descriptor for `slot`.
static void write_image_descriptor(unsigned slot, const DkImage *image) {
    DkImageView view;
    dkImageViewDefaults(&view, image);
    DkImageDescriptor desc;
    dkImageDescriptorInitialize(&desc, &view, false, false);
    void *dst = (uint8_t *)dkMemBlockGetCpuAddr(g_descMemBlock) + DESC_IMG_OFFSET(slot);
    memcpy(dst, &desc, sizeof(desc));
}

static DkMemBlock make_memblock(uint32_t size, uint32_t flags) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, g_device, align_up(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
}

// (Re)initializes `image`/`*mem` at exactly w x h, allocating backing memory
// the first time and reinitializing the DkImage in place on later calls
// (same memblock, same offset — just a new logical layout), then rewrites its
// descriptor. Returns false if w/h are out of range or allocation fails.
static bool init_or_resize_image(DkImage *image, DkMemBlock *mem, unsigned w, unsigned h,
                                  unsigned max_w, unsigned max_h, unsigned slot, bool first_time,
                                  uint32_t extra_flags) {
    if (w == 0 || h == 0 || w > max_w || h > max_h) return false;

    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, g_device);
    lm.flags = extra_flags;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);

    if (first_time) {
        uint32_t size = align_up((uint32_t)dkImageLayoutGetSize(&layout),
                                  dkImageLayoutGetAlignment(&layout));
        *mem = make_memblock(size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
        if (!*mem) return false;
    }
    dkImageInitialize(image, &layout, *mem, 0);
    write_image_descriptor(slot, image);
    return true;
}

// --- teardown --------------------------------------------------------------
// Mirrors init order in reverse; every field is safe to destroy-if-nonzero so
// this doubles as the partial-failure unwind path during gpu_video_init.
static void teardown(void) {
    if (g_queue) dkQueueWaitIdle(g_queue);
    if (g_cmdbuf) dkCmdBufDestroy(g_cmdbuf);
    if (g_cmdbufMemBlock) dkMemBlockDestroy(g_cmdbufMemBlock);
    if (g_descMemBlock) dkMemBlockDestroy(g_descMemBlock);
    for (int i = 0; i < N_PARITY; i++) {
        if (g_overlayScratch[i]) dkMemBlockDestroy(g_overlayScratch[i]);
        if (g_hudScratch[i]) dkMemBlockDestroy(g_hudScratch[i]);
        if (g_gameScratch[i]) dkMemBlockDestroy(g_gameScratch[i]);
        if (g_networkScratch[i]) dkMemBlockDestroy(g_networkScratch[i]);
    }
    if (g_overlayImgMem) dkMemBlockDestroy(g_overlayImgMem);
    if (g_hudImgMem) dkMemBlockDestroy(g_hudImgMem);
    if (g_gameImgMem) dkMemBlockDestroy(g_gameImgMem);
    if (g_espcnOutImgMem) dkMemBlockDestroy(g_espcnOutImgMem);
    if (g_espcnFeat1Mem) dkMemBlockDestroy(g_espcnFeat1Mem);
    if (g_espcnFeat2Mem) dkMemBlockDestroy(g_espcnFeat2Mem);
    if (g_espcnDimsUbo) dkMemBlockDestroy(g_espcnDimsUbo);
    if (g_espcnWeights) dkMemBlockDestroy(g_espcnWeights);
    if (g_codeMemBlock) dkMemBlockDestroy(g_codeMemBlock);
    if (g_swapchain) dkSwapchainDestroy(g_swapchain);
    if (g_fbMemBlock) dkMemBlockDestroy(g_fbMemBlock);
    if (g_queue) dkQueueDestroy(g_queue);
    if (g_device) dkDeviceDestroy(g_device);
    g_device = NULL;
    g_queue = NULL;
    g_fbMemBlock = NULL;
    g_swapchain = NULL;
    g_codeMemBlock = NULL;
    g_gameImgMem = g_hudImgMem = g_overlayImgMem = NULL;
    g_espcnOutImgMem = g_espcnFeat1Mem = g_espcnFeat2Mem = g_espcnDimsUbo = g_espcnWeights = NULL;
    g_espcn_available = false;
    memset(g_gameScratch, 0, sizeof(g_gameScratch));
    memset(g_hudScratch, 0, sizeof(g_hudScratch));
    memset(g_overlayScratch, 0, sizeof(g_overlayScratch));
    memset(g_networkScratch, 0, sizeof(g_networkScratch));
    g_descMemBlock = NULL;
    g_cmdbufMemBlock = NULL;
    g_cmdbuf = NULL;
}

static bool try_init_espcn(void);  // defined below; called from gpu_video_init

bool gpu_video_init(NWindow *win) {
    g_device = NULL;
    g_queue = NULL;
    g_fbMemBlock = NULL;
    g_swapchain = NULL;
    g_codeMemBlock = NULL;
    g_gameImgMem = g_hudImgMem = g_overlayImgMem = NULL;
    memset(g_gameScratch, 0, sizeof(g_gameScratch));
    memset(g_hudScratch, 0, sizeof(g_hudScratch));
    memset(g_overlayScratch, 0, sizeof(g_overlayScratch));
    memset(g_networkScratch, 0, sizeof(g_networkScratch));
    g_descMemBlock = NULL;
    g_cmdbufMemBlock = NULL;
    g_cmdbuf = NULL;
    g_game_w = g_game_h = 0;
    g_hud_visible = g_overlay_visible = false;
    g_crt_enabled = false;
    g_game_pending = g_hud_pending = g_overlay_pending = false;
    g_parity = 0;
    g_espcn_available = false;
    g_espcn_enabled = false;
    g_espcnWeights = g_espcnDimsUbo = g_espcnFeat1Mem = g_espcnFeat2Mem = g_espcnOutImgMem = NULL;
    g_espcn_last_us = 0;
    g_espcn_have_output = false;
    g_network_enabled = false;
    g_network_have_output = false;
    g_network_pending = false;
    g_win = win;
    resolution_for_mode(&g_fb_w, &g_fb_h);  // start at whatever mode we're already in
    compute_dst_layout();

    DkDeviceMaker devMaker;
    dkDeviceMakerDefaults(&devMaker);
    g_device = dkDeviceCreate(&devMaker);
    if (!g_device) return false;

    DkQueueMaker qMaker;
    dkQueueMakerDefaults(&qMaker, g_device);
    // Compute must be explicitly requested — dkQueueMakerDefaults already
    // includes it, but the original (pre-ESPCN) code here overwrote flags
    // with Graphics only, silently dropping it.
    qMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;
    // The default perWarpScratchMemorySize (4 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT
    // = 2KB) is sized for this file's tiny vertex/fragment shaders. The ESPCN
    // compute shaders unroll deep nested loops (64ch/32ch convolutions), which
    // need far more per-warp scratch register space — too little of it is
    // exactly "not enough scratch memory to run compute shaders" from the
    // debug validation layer, a fatal dkQueueSubmitCommands abort on real
    // hardware. 16x headroom over the default.
    qMaker.perWarpScratchMemorySize = 16 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT;
    g_queue = dkQueueCreate(&qMaker);
    if (!g_queue) { teardown(); return false; }

    // Swapchain framebuffers, sized to the current dock/handheld resolution;
    // gpu_video_present() polls for mode changes and calls resize_swapchain().
    DkImageLayoutMaker fbLm;
    dkImageLayoutMakerDefaults(&fbLm, g_device);
    fbLm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    fbLm.format = DkImageFormat_RGBA8_Unorm;
    fbLm.dimensions[0] = g_fb_w;
    fbLm.dimensions[1] = g_fb_h;
    DkImageLayout fbLayout;
    dkImageLayoutInitialize(&fbLayout, &fbLm);
    uint32_t fbSize = align_up((uint32_t)dkImageLayoutGetSize(&fbLayout),
                                dkImageLayoutGetAlignment(&fbLayout));
    g_fbMemBlock = make_memblock(fbSize * N_FB, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!g_fbMemBlock) { teardown(); return false; }
    DkImage const *fbPtrs[N_FB];
    for (int i = 0; i < N_FB; i++) {
        fbPtrs[i] = &g_fbImages[i];
        dkImageInitialize(&g_fbImages[i], &fbLayout, g_fbMemBlock, i * fbSize);
    }

    DkSwapchainMaker scMaker;
    dkSwapchainMakerDefaults(&scMaker, g_device, win, fbPtrs, N_FB);
    g_swapchain = dkSwapchainCreate(&scMaker);
    if (!g_swapchain) { teardown(); return false; }

    // Shaders.
    g_codeMemBlock = make_memblock(CODE_MEM_SIZE, DkMemBlockFlags_CpuUncached |
                                    DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    if (!g_codeMemBlock) { teardown(); return false; }
    g_codeMemOffset = 0;
    if (!load_shader(&g_vsh, "romfs:/shaders/fullscreen_vsh.dksh") ||
        !load_shader(&g_fsh, "romfs:/shaders/texture_fsh.dksh") ||
        !load_shader(&g_crtFsh, "romfs:/shaders/crt_fsh.dksh")) {
        teardown();
        return false;
    }

    // Descriptor memory (3 image descriptors + 1 sampler descriptor).
    g_descMemBlock = make_memblock(DK_MEMBLOCK_ALIGNMENT,
                                    DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!g_descMemBlock) { teardown(); return false; }

    // Game/HUD/overlay textures + their double-buffered CPU-visible staging.
    if (!init_or_resize_image(&g_gameImage, &g_gameImgMem, GAME_MAX_W, GAME_MAX_H,
                               GAME_MAX_W, GAME_MAX_H, IMG_GAME, true, 0)) { teardown(); return false; }
    g_game_w = g_game_h = 0;  // force a real (re)init on the first real upload
    if (!init_or_resize_image(&g_hudImage, &g_hudImgMem, HUD_W, HUD_H,
                               HUD_W, HUD_H, IMG_HUD, true, 0)) { teardown(); return false; }
    if (!init_or_resize_image(&g_overlayImage, &g_overlayImgMem, OVERLAY_W, OVERLAY_H,
                               OVERLAY_W, OVERLAY_H, IMG_OVERLAY, true, 0)) { teardown(); return false; }

    uint32_t gameScratchSize = GAME_MAX_W * GAME_MAX_H * 4;
    uint32_t hudScratchSize = HUD_W * HUD_H * 4;
    uint32_t overlayScratchSize = OVERLAY_W * OVERLAY_H * 4;
    uint32_t networkScratchSize = (ESPCN_MAX_W * ESPCN_SCALE) * (ESPCN_MAX_H * ESPCN_SCALE) * 4;
    for (int i = 0; i < N_PARITY; i++) {
        g_gameScratch[i] = make_memblock(gameScratchSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        g_hudScratch[i] = make_memblock(hudScratchSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        g_overlayScratch[i] = make_memblock(overlayScratchSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        g_networkScratch[i] = make_memblock(networkScratchSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        if (!g_gameScratch[i] || !g_hudScratch[i] || !g_overlayScratch[i] || !g_networkScratch[i]) { teardown(); return false; }
    }

    // Sampler: nearest filtering (the default) is exactly the CPU path's
    // nearest-neighbor scaling behavior; clamp so the not-fully-used game/HUD
    // textures never bleed their edge texels from unused padding.
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
    DkSamplerDescriptor sdesc;
    dkSamplerDescriptorInitialize(&sdesc, &sampler);
    memcpy((uint8_t *)dkMemBlockGetCpuAddr(g_descMemBlock) + DESC_SAMPLER_OFFSET, &sdesc, sizeof(sdesc));

    // Experimental AI upscale — optional, see try_init_espcn's comment. Its
    // absence is not a gpu_video_init failure.
    g_espcn_available = try_init_espcn();

    // Command buffer, reused (cleared + re-recorded) every present().
    g_cmdbufMemBlock = make_memblock(CMD_MEM_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!g_cmdbufMemBlock) { teardown(); return false; }
    DkCmdBufMaker cbMaker;
    dkCmdBufMakerDefaults(&cbMaker, g_device);
    g_cmdbuf = dkCmdBufCreate(&cbMaker);
    if (!g_cmdbuf) { teardown(); return false; }
    dkCmdBufAddMemory(g_cmdbuf, g_cmdbufMemBlock, 0, CMD_MEM_SIZE);

    // One-time setup: bind the descriptor sets. Per deko3d's documented usage
    // (mirrored from the SDK samples), the bound descriptor-set *addresses*
    // persist on the queue across all later, separately-recorded command
    // lists — this is not rebound per frame.
    dkCmdBufBindImageDescriptorSet(g_cmdbuf, dkMemBlockGetGpuAddr(g_descMemBlock), IMG_COUNT);
    dkCmdBufBindSamplerDescriptorSet(g_cmdbuf, dkMemBlockGetGpuAddr(g_descMemBlock) + DESC_SAMPLER_OFFSET, 1);
    DkCmdList setupList = dkCmdBufFinishList(g_cmdbuf);
    dkQueueSubmitCommands(g_queue, setupList);
    dkQueueWaitIdle(g_queue);
    // gpu_video_present() does its own clear+addMemory pairing at the top of
    // every call (including the first), so no clear is needed here.

    g_ready = true;
    return true;
}

void gpu_video_upload_frame(const uint32_t *rgba8, unsigned w, unsigned h) {
    if (!g_ready || w == 0 || h == 0 || w > GAME_MAX_W || h > GAME_MAX_H) return;
    if (w != g_game_w || h != g_game_h) {
        // Rare (core geometry change): stall once to safely respecify the
        // image's logical layout in place, rather than reasoning about an
        // in-flight command list still referencing the old layout.
        dkQueueWaitIdle(g_queue);
        if (!init_or_resize_image(&g_gameImage, &g_gameImgMem, w, h,
                                   GAME_MAX_W, GAME_MAX_H, IMG_GAME, false, 0))
            return;
        g_game_w = w; g_game_h = h;
    }
    void *dst = dkMemBlockGetCpuAddr(g_gameScratch[g_parity]);
    memcpy(dst, rgba8, (size_t)w * h * sizeof(uint32_t));
    g_pending_game_w = w; g_pending_game_h = h;
    g_game_pending = true;
}

void gpu_video_set_hud(const uint32_t *rgba8, unsigned w, unsigned h) {
    if (!g_ready) return;
    if (!rgba8 || w != HUD_W || h != HUD_H) { g_hud_visible = false; return; }
    void *dst = dkMemBlockGetCpuAddr(g_hudScratch[g_parity]);
    memcpy(dst, rgba8, (size_t)HUD_W * HUD_H * sizeof(uint32_t));
    g_hud_pending = true;
    g_hud_visible = true;
}

void gpu_video_set_overlay(const uint32_t *rgba_1280x720) {
    if (!g_ready) return;
    if (!rgba_1280x720) { g_overlay_visible = false; return; }
    void *dst = dkMemBlockGetCpuAddr(g_overlayScratch[g_parity]);
    memcpy(dst, rgba_1280x720, (size_t)OVERLAY_W * OVERLAY_H * sizeof(uint32_t));
    g_overlay_pending = true;
    g_overlay_visible = true;
}

void gpu_video_set_crt(bool enabled) {
    if (!g_ready) return;
    g_crt_enabled = enabled;
}

void gpu_video_get_resolution(unsigned *w, unsigned *h) {
    *w = g_fb_w;
    *h = g_fb_h;
}

// Draws one full-screen-quad-shader textured rect into the given viewport,
// sampling the image bound at `slot`. Shared by the game/HUD/overlay draws —
// they differ only in which image slot and screen rect they use.
static void draw_textured_quad(unsigned slot, int x, int y, int w, int h) {
    DkViewport vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    DkScissor sc = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    dkCmdBufSetViewports(g_cmdbuf, 0, &vp, 1);
    dkCmdBufSetScissors(g_cmdbuf, 0, &sc, 1);
    dkCmdBufBindTexture(g_cmdbuf, DkStage_Fragment, 0, dkMakeTextureHandle(slot, SAMPLER_SLOT));
    dkCmdBufDraw(g_cmdbuf, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

// Best-effort: loads a local, gitignored weights file (see settings.h — the
// pretrained weights' license is unresolved, so this is never shipped; its
// absence here is the normal case for anyone besides local development) and,
// only if present, allocates everything the ESPCN compute passes need.
// Failure at any point cleans up whatever THIS function allocated and leaves
// g_espcn_available false — every other GPU feature works identically
// whether or not this succeeds, since it's never on the teardown()/fatal path.
static bool try_init_espcn(void) {
    FILE *f = fopen(ESPCN_WEIGHTS_PATH, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 56, SEEK_SET);  // skip magic(4)+scale(4)+3x dims(4x4 each) header
    long payload = fsize - 56;
    if (payload != (long)(ESPCN_WEIGHTS_FLOATS * sizeof(float))) { fclose(f); return false; }

    if (!load_shader(&g_espcn1, "romfs:/shaders/espcn1_comp.dksh") ||
        !load_shader(&g_espcn2, "romfs:/shaders/espcn2_comp.dksh") ||
        !load_shader(&g_espcn3, "romfs:/shaders/espcn3_comp.dksh")) {
        fclose(f);
        return false;
    }

    g_espcnWeights = make_memblock(ESPCN_WEIGHTS_FLOATS * sizeof(float),
                                    DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!g_espcnWeights) { fclose(f); return false; }
    bool ok = fread(dkMemBlockGetCpuAddr(g_espcnWeights), 1, (size_t)payload, f) == (size_t)payload;
    fclose(f);
    if (!ok) { dkMemBlockDestroy(g_espcnWeights); g_espcnWeights = NULL; return false; }

    g_espcnDimsUbo = make_memblock(DK_UNIFORM_BUF_ALIGNMENT,
                                    DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!g_espcnDimsUbo) { dkMemBlockDestroy(g_espcnWeights); g_espcnWeights = NULL; return false; }

    uint32_t feat1Size = 64u * ESPCN_MAX_H * ESPCN_MAX_W * sizeof(float);
    uint32_t feat2Size = 32u * ESPCN_MAX_H * ESPCN_MAX_W * sizeof(float);
    g_espcnFeat1Mem = make_memblock(feat1Size, DkMemBlockFlags_GpuCached);
    g_espcnFeat2Mem = make_memblock(feat2Size, DkMemBlockFlags_GpuCached);
    if (!g_espcnFeat1Mem || !g_espcnFeat2Mem) {
        if (g_espcnFeat1Mem) dkMemBlockDestroy(g_espcnFeat1Mem);
        if (g_espcnFeat2Mem) dkMemBlockDestroy(g_espcnFeat2Mem);
        g_espcnFeat1Mem = g_espcnFeat2Mem = NULL;
        dkMemBlockDestroy(g_espcnDimsUbo); g_espcnDimsUbo = NULL;
        dkMemBlockDestroy(g_espcnWeights); g_espcnWeights = NULL;
        return false;
    }

    if (!init_or_resize_image(&g_espcnOutImage, &g_espcnOutImgMem,
                               ESPCN_MAX_W * ESPCN_SCALE, ESPCN_MAX_H * ESPCN_SCALE,
                               ESPCN_MAX_W * ESPCN_SCALE, ESPCN_MAX_H * ESPCN_SCALE,
                               IMG_ESPCN_OUT, true, DkImageFlags_UsageLoadStore)) {
        dkMemBlockDestroy(g_espcnFeat1Mem); g_espcnFeat1Mem = NULL;
        dkMemBlockDestroy(g_espcnFeat2Mem); g_espcnFeat2Mem = NULL;
        dkMemBlockDestroy(g_espcnDimsUbo); g_espcnDimsUbo = NULL;
        dkMemBlockDestroy(g_espcnWeights); g_espcnWeights = NULL;
        return false;
    }

    return true;
}

// Recreates the swapchain + its framebuffer images at new_w x new_h (a real
// dock/handheld transition). Builds the new swapchain BEFORE touching the old
// one, so a failure here (allocation, API error) just leaves the old
// resolution running rather than risking ending up with no swapchain at all —
// only commits to the new state once every step has actually succeeded.
static void resize_swapchain(unsigned new_w, unsigned new_h) {
    DkImageLayoutMaker fbLm;
    dkImageLayoutMakerDefaults(&fbLm, g_device);
    fbLm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    fbLm.format = DkImageFormat_RGBA8_Unorm;
    fbLm.dimensions[0] = new_w;
    fbLm.dimensions[1] = new_h;
    DkImageLayout fbLayout;
    dkImageLayoutInitialize(&fbLayout, &fbLm);
    uint32_t fbSize = align_up((uint32_t)dkImageLayoutGetSize(&fbLayout),
                                dkImageLayoutGetAlignment(&fbLayout));

    DkMemBlock newMem = make_memblock(fbSize * N_FB, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!newMem) return;  // keep running at the old resolution

    DkImage const *newPtrs[N_FB];
    for (int i = 0; i < N_FB; i++) {
        newPtrs[i] = &g_fbImagesStaging[i];
        dkImageInitialize(&g_fbImagesStaging[i], &fbLayout, newMem, i * fbSize);
    }

    DkSwapchainMaker scMaker;
    dkSwapchainMakerDefaults(&scMaker, g_device, g_win, newPtrs, N_FB);
    DkSwapchain newSwapchain = dkSwapchainCreate(&scMaker);
    if (!newSwapchain) { dkMemBlockDestroy(newMem); return; }  // keep running at the old resolution

    // New resources are confirmed working — safe to drop the old ones now.
    dkSwapchainDestroy(g_swapchain);
    dkMemBlockDestroy(g_fbMemBlock);

    g_swapchain = newSwapchain;
    g_fbMemBlock = newMem;
    memcpy(g_fbImages, g_fbImagesStaging, sizeof(g_fbImages));
    g_fb_w = new_w;
    g_fb_h = new_h;
    compute_dst_layout();
}

void gpu_video_set_ai_upscale(bool enabled) {
    if (!g_ready) return;
    if (enabled && !g_espcn_enabled) g_espcn_have_output = false;  // force a fresh
                                      // dispatch on the next present rather than
                                      // reusing whatever (possibly long-stale, or
                                      // never-written) output was last there.
    g_espcn_enabled = enabled;
}

bool gpu_video_ai_upscale_active(void) {
    return g_espcn_available && g_espcn_enabled &&
           g_game_w > 0 && g_game_w <= ESPCN_MAX_W && g_game_h <= ESPCN_MAX_H;
}

unsigned gpu_video_get_ai_upscale_us(void) {
    return g_espcn_last_us;
}

bool gpu_video_ai_upscale_available(void) {
    return g_espcn_available;
}

void gpu_video_set_network_upscale(bool enabled) {
    if (!g_ready) return;
    if (enabled && !g_network_enabled) g_network_have_output = false;  // see
                                        // gpu_video_set_ai_upscale's identical
                                        // reasoning: force a fresh result
                                        // rather than reusing a stale one.
    g_network_enabled = enabled;
}

bool gpu_video_network_upscale_active(void) {
    return g_network_enabled && g_network_have_output;
}

// Recombines a network-upscaled luma plane with the original small frame's
// color into RGB (same YCbCr math as espcn3_comp.glsl's tail, just run on
// the CPU instead of the GPU) and stages it for upload into the same output
// slot the local compute path uses. w/h must exactly match the current game
// resolution scaled by ESPCN_SCALE — anything else is a stale response for
// a since-changed resolution and is dropped rather than risking sampling
// g_gameScratch out of bounds.
// Per-source-pixel chroma offsets for the network-result recombine (below).
// Each source pixel's chroma is shared by its whole 3x3 output block, so
// this is 9x less chroma math than computing it per output pixel — and it
// also means the CPU-uncached game scratch gets read once per source pixel
// instead of nine times (uncached reads are painfully slow on this CPU;
// the original per-output-pixel float version of this function measured in
// the ~100ms range and single-handedly capped the whole game loop at ~6fps).
static int16_t s_chroma_dr[ESPCN_MAX_W * ESPCN_MAX_H];
static int16_t s_chroma_dg[ESPCN_MAX_W * ESPCN_MAX_H];
static int16_t s_chroma_db[ESPCN_MAX_W * ESPCN_MAX_H];

void gpu_video_upload_network_result(const uint8_t *luma, unsigned w, unsigned h) {
    if (!g_ready || !luma || !g_network_enabled) return;
    if (g_game_w == 0 || w != g_game_w * ESPCN_SCALE || h != g_game_h * ESPCN_SCALE) return;
    if (w > ESPCN_MAX_W * ESPCN_SCALE || h > ESPCN_MAX_H * ESPCN_SCALE) return;

    const uint32_t *src = (const uint32_t *)dkMemBlockGetCpuAddr(g_gameScratch[g_parity]);
    uint32_t *dst = (uint32_t *)dkMemBlockGetCpuAddr(g_networkScratch[g_parity]);

    // Integer fixed-point YCbCr recombine, two passes. All coefficients are
    // the same BT.601 constants the float version (and espcn3_comp.glsl's
    // shader tail) uses, scaled by 256 and rounded — worst-case error vs the
    // float math is ~1 LSB, far below anything visible.
    //
    // Pass 1: chroma offsets per SOURCE pixel. cbx/crx are (cb-0.5) and
    // (cr-0.5) in 255-scale, x256; dr/dg/db are the RGB deltas to add to the
    // new luma, in plain pixel units (range ±~180, comfortably s16).
    unsigned gw = g_game_w, gh = g_game_h;
    for (unsigned i = 0; i < gw * gh; i++) {
        uint32_t px = src[i];
        int r = (int)(px & 0xFF), g = (int)((px >> 8) & 0xFF), b = (int)((px >> 16) & 0xFF);
        int32_t cbx = -43 * r - 85 * g + 128 * b;   // 256*(-0.168736 r -0.331264 g +0.5 b)
        int32_t crx = 128 * r - 107 * g - 21 * b;   // 256*(0.5 r -0.418688 g -0.081312 b)
        s_chroma_dr[i] = (int16_t)((359 * crx + 32768) >> 16);              // 1.402 (cr-.5)
        s_chroma_dg[i] = (int16_t)((-88 * cbx - 183 * crx + 32768) >> 16);  // -.344 cb -.714 cr
        s_chroma_db[i] = (int16_t)((454 * cbx + 32768) >> 16);              // 1.772 (cb-.5)
    }

    // Pass 2: per output pixel, just luma + offset + clamp + pack — a
    // handful of integer ops, no divides, no floats, sequential access.
    for (unsigned sy = 0; sy < gh; sy++) {
        const int16_t *drr = s_chroma_dr + (size_t)sy * gw;
        const int16_t *dgr = s_chroma_dg + (size_t)sy * gw;
        const int16_t *dbr = s_chroma_db + (size_t)sy * gw;
        for (unsigned oy = sy * ESPCN_SCALE; oy < sy * ESPCN_SCALE + ESPCN_SCALE; oy++) {
            const uint8_t *lrow = luma + (size_t)oy * w;
            uint32_t *drow = dst + (size_t)oy * w;
            for (unsigned sx = 0; sx < gw; sx++) {
                int dr = drr[sx], dg = dgr[sx], db = dbr[sx];
                unsigned ox = sx * ESPCN_SCALE;
                for (unsigned k = 0; k < ESPCN_SCALE; k++, ox++) {
                    int y = (int)lrow[ox];
                    int r = y + dr, g = y + dg, b = y + db;
                    r = r < 0 ? 0 : (r > 255 ? 255 : r);
                    g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b);
                    drow[ox] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
                }
            }
        }
    }
    g_network_pending_w = w;
    g_network_pending_h = h;
    g_network_pending = true;
}

// Runs the 3 ESPCN compute passes (espcn1/2/3_comp.glsl) against the current
// game frame, writing the result to g_espcnOutImage. Deliberately its own
// synchronous submission rather than folded into the main per-frame command
// list: keeps its cost independently measurable (g_espcn_last_us, surfaced
// via gpu_video_get_ai_upscale_us for the HUD) and avoids reasoning about
// compute/graphics barriers sharing a single list for a first, unverified cut.
static void run_espcn_upscale(void) {
    u64 t0 = armGetSystemTick();

    struct { uint32_t w, h; } dims = { g_game_w, g_game_h };
    memcpy(dkMemBlockGetCpuAddr(g_espcnDimsUbo), &dims, sizeof(dims));

    dkCmdBufClear(g_cmdbuf);
    dkCmdBufAddMemory(g_cmdbuf, g_cmdbufMemBlock, 0, CMD_MEM_SIZE);

    const uint32_t weightsBytes = ESPCN_WEIGHTS_FLOATS * sizeof(float);
    const uint32_t feat1Bytes = 64u * ESPCN_MAX_H * ESPCN_MAX_W * sizeof(float);
    const uint32_t feat2Bytes = 32u * ESPCN_MAX_H * ESPCN_MAX_W * sizeof(float);
    const DkGpuAddr dimsAddr = dkMemBlockGetGpuAddr(g_espcnDimsUbo);
    const DkGpuAddr weightsAddr = dkMemBlockGetGpuAddr(g_espcnWeights);
    // Cross-stage producer/consumer barrier: combine both invalidate flags
    // since it's unverified whether SSBO traffic goes through the same cache
    // path as the image/texture barriers used elsewhere in this file — extra
    // invalidation is wasted cycles at worst, never incorrect.
    const uint32_t barrierFlags = DkInvalidateFlags_L2Cache | DkInvalidateFlags_Image;
    unsigned gx = (g_game_w + 7) / 8, gy = (g_game_h + 7) / 8;

    // Layer 1: source RGB (luma extracted in-shader) -> feat1 (64ch)
    DkShader const *l1[] = { &g_espcn1 };
    dkCmdBufBindShaders(g_cmdbuf, DkStageFlag_Compute, l1, 1);
    dkCmdBufBindTexture(g_cmdbuf, DkStage_Compute, 0, dkMakeTextureHandle(IMG_GAME, SAMPLER_SLOT));
    dkCmdBufBindUniformBuffer(g_cmdbuf, DkStage_Compute, 0, dimsAddr, DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindStorageBuffer(g_cmdbuf, DkStage_Compute, 0, weightsAddr, weightsBytes);
    dkCmdBufBindStorageBuffer(g_cmdbuf, DkStage_Compute, 1, dkMemBlockGetGpuAddr(g_espcnFeat1Mem), feat1Bytes);
    dkCmdBufDispatchCompute(g_cmdbuf, gx, gy, 1);
    dkCmdBufBarrier(g_cmdbuf, DkBarrier_Full, barrierFlags);

    // Layer 2: feat1 (64ch) -> feat2 (32ch)
    DkShader const *l2[] = { &g_espcn2 };
    dkCmdBufBindShaders(g_cmdbuf, DkStageFlag_Compute, l2, 1);
    dkCmdBufBindUniformBuffer(g_cmdbuf, DkStage_Compute, 0, dimsAddr, DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindStorageBuffer(g_cmdbuf, DkStage_Compute, 0, weightsAddr, weightsBytes);
    dkCmdBufBindStorageBuffer(g_cmdbuf, DkStage_Compute, 1, dkMemBlockGetGpuAddr(g_espcnFeat1Mem), feat1Bytes);
    dkCmdBufBindStorageBuffer(g_cmdbuf, DkStage_Compute, 2, dkMemBlockGetGpuAddr(g_espcnFeat2Mem), feat2Bytes);
    dkCmdBufDispatchCompute(g_cmdbuf, gx, gy, 1);
    dkCmdBufBarrier(g_cmdbuf, DkBarrier_Full, barrierFlags);

    // Layer 3 + pixel shuffle + YCbCr recombine -> g_espcnOutImage (3x size)
    DkShader const *l3[] = { &g_espcn3 };
    dkCmdBufBindShaders(g_cmdbuf, DkStageFlag_Compute, l3, 1);
    dkCmdBufBindTexture(g_cmdbuf, DkStage_Compute, 0, dkMakeTextureHandle(IMG_GAME, SAMPLER_SLOT));
    dkCmdBufBindImage(g_cmdbuf, DkStage_Compute, 0, dkMakeImageHandle(IMG_ESPCN_OUT));
    dkCmdBufBindUniformBuffer(g_cmdbuf, DkStage_Compute, 0, dimsAddr, DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindStorageBuffer(g_cmdbuf, DkStage_Compute, 0, weightsAddr, weightsBytes);
    dkCmdBufBindStorageBuffer(g_cmdbuf, DkStage_Compute, 1, dkMemBlockGetGpuAddr(g_espcnFeat2Mem), feat2Bytes);
    dkCmdBufDispatchCompute(g_cmdbuf, gx, gy, 1);
    // This barrier matters more than the previous two: the very next thing
    // that happens is the main draw sampling g_espcnOutImage as a texture.
    dkCmdBufBarrier(g_cmdbuf, DkBarrier_Full, barrierFlags);

    DkCmdList list = dkCmdBufFinishList(g_cmdbuf);
    dkQueueSubmitCommands(g_queue, list);
    dkQueueWaitIdle(g_queue);  // isolates the timing measurement; also leaves
                               // the queue idle before the main cmdbuf below
                               // reuses g_cmdbuf's memory, per the existing
                               // clear+addMemory safety invariant.

    g_espcn_last_us = (unsigned)(armTicksToNs(armGetSystemTick() - t0) / 1000);
}

void gpu_video_present(void) {
    if (!g_ready) return;

    // Cheap to poll every frame; only actually resizes on a real dock/handheld
    // change. Safe to touch swapchain resources here with no extra wait: the
    // GPU is guaranteed idle at this point, either from init's own wait or
    // from the trailing dkQueueWaitIdle at the end of every previous call.
    unsigned want_w, want_h;
    resolution_for_mode(&want_w, &want_h);
    if (want_w != g_fb_w || want_h != g_fb_h) resize_swapchain(want_w, want_h);

    bool use_espcn = gpu_video_ai_upscale_active();
    // Skip the recompute on ticks with no new game frame (the SNES core
    // frequently re-presents the same frame while pacing to audio — see
    // main.c's dupe-frame tracking). g_espcnOutImage still holds last time's
    // result, which is exactly correct to reuse since the source pixels
    // (g_gameImage) haven't changed either.
    if (use_espcn && (g_game_pending || !g_espcn_have_output)) {
        run_espcn_upscale();
        g_espcn_have_output = true;
    }

    int slot = dkQueueAcquireImage(g_queue, g_swapchain);

    // dkCmdBufClear() detaches the buffer's backing memory as well as
    // discarding recorded commands (confirmed by deko3d's own CCmdMemRing
    // sample) — it must always be immediately followed by dkCmdBufAddMemory
    // before recording anything, or the next command lands in a buffer with
    // no memory behind it.
    dkCmdBufClear(g_cmdbuf);
    dkCmdBufAddMemory(g_cmdbuf, g_cmdbufMemBlock, 0, CMD_MEM_SIZE);

    // Upload whatever changed since the last present, then a single barrier
    // before any of this frame's draws sample those images (copy-engine
    // writes aren't automatically visible to the texture unit otherwise).
    bool any_pending = g_game_pending || g_hud_pending || g_overlay_pending || g_network_pending;
    if (g_game_pending) {
        DkCopyBuf src = { dkMemBlockGetGpuAddr(g_gameScratch[g_parity]), 0, 0 };
        DkImageView view;
        dkImageViewDefaults(&view, &g_gameImage);
        DkImageRect rect = { 0, 0, 0, g_pending_game_w, g_pending_game_h, 1 };
        dkCmdBufCopyBufferToImage(g_cmdbuf, &src, &view, &rect, 0);
    }
    if (g_network_pending) {
        // Same output slot the local ESPCN compute path writes — see
        // gpu_video.h. Uploaded via the CPU-recombined buffer instead of a
        // compute dispatch.
        DkCopyBuf src = { dkMemBlockGetGpuAddr(g_networkScratch[g_parity]), 0, 0 };
        DkImageView view;
        dkImageViewDefaults(&view, &g_espcnOutImage);
        DkImageRect rect = { 0, 0, 0, g_network_pending_w, g_network_pending_h, 1 };
        dkCmdBufCopyBufferToImage(g_cmdbuf, &src, &view, &rect, 0);
        g_network_have_output = true;
        g_network_pending = false;
    }
    if (g_hud_pending) {
        DkCopyBuf src = { dkMemBlockGetGpuAddr(g_hudScratch[g_parity]), 0, 0 };
        DkImageView view;
        dkImageViewDefaults(&view, &g_hudImage);
        DkImageRect rect = { 0, 0, 0, HUD_W, HUD_H, 1 };
        dkCmdBufCopyBufferToImage(g_cmdbuf, &src, &view, &rect, 0);
    }
    if (g_overlay_pending) {
        DkCopyBuf src = { dkMemBlockGetGpuAddr(g_overlayScratch[g_parity]), 0, 0 };
        DkImageView view;
        dkImageViewDefaults(&view, &g_overlayImage);
        DkImageRect rect = { 0, 0, 0, OVERLAY_W, OVERLAY_H, 1 };
        dkCmdBufCopyBufferToImage(g_cmdbuf, &src, &view, &rect, 0);
    }
    if (any_pending) dkCmdBufBarrier(g_cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);

    DkImageView fbView;
    dkImageViewDefaults(&fbView, &g_fbImages[slot]);
    dkCmdBufBindRenderTarget(g_cmdbuf, &fbView, NULL);

    DkViewport fullVp = { 0.0f, 0.0f, (float)g_fb_w, (float)g_fb_h, 0.0f, 1.0f };
    DkScissor fullSc = { 0, 0, g_fb_w, g_fb_h };
    dkCmdBufSetViewports(g_cmdbuf, 0, &fullVp, 1);
    dkCmdBufSetScissors(g_cmdbuf, 0, &fullSc, 1);
    dkCmdBufClearColorFloat(g_cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

    DkShader const *plainShaders[] = { &g_vsh, &g_fsh };
    DkShader const *crtShaders[] = { &g_vsh, &g_crtFsh };
    dkCmdBufBindShaders(g_cmdbuf, DkStageFlag_GraphicsMask, plainShaders, 2);
    DkRasterizerState rs; dkRasterizerStateDefaults(&rs);
    DkColorState cs; dkColorStateDefaults(&cs);
    DkColorWriteState cws; dkColorWriteStateDefaults(&cws);
    dkCmdBufBindRasterizerState(g_cmdbuf, &rs);
    dkCmdBufBindColorState(g_cmdbuf, &cs);
    dkCmdBufBindColorWriteState(g_cmdbuf, &cws);

    if (g_game_w && g_game_h) {
        // CRT look applies only to the game quad — HUD/menu overlay stay on
        // the plain shader, so rebind around just this draw.
        if (g_crt_enabled) dkCmdBufBindShaders(g_cmdbuf, DkStageFlag_GraphicsMask, crtShaders, 2);
        unsigned gameSrc = (use_espcn || gpu_video_network_upscale_active()) ? IMG_ESPCN_OUT : IMG_GAME;
        draw_textured_quad(gameSrc, g_dst_x0, g_dst_y0, (int)g_dst_w, (int)g_dst_h);
        if (g_crt_enabled) dkCmdBufBindShaders(g_cmdbuf, DkStageFlag_GraphicsMask, plainShaders, 2);
    }
    if (g_hud_visible)
        draw_textured_quad(IMG_HUD, 16, 16, HUD_W, HUD_H);
    if (g_overlay_visible)
        draw_textured_quad(IMG_OVERLAY, 0, 0, (int)g_fb_w, (int)g_fb_h);

    DkCmdList list = dkCmdBufFinishList(g_cmdbuf);
    dkQueueSubmitCommands(g_queue, list);
    dkQueuePresentImage(g_queue, g_swapchain, slot);

    // g_cmdbuf's backing memory is a single region re-added every frame (see
    // the clear+addMemory pairing above); without waiting here, the next
    // frame's dkCmdBufClear/AddMemory could start overwriting command data
    // the GPU hasn't finished reading yet from this frame's async submission
    // (deko3d's own CCmdMemRing sample exists specifically to avoid this via
    // a fenced multi-slice ring — this is the simpler, unambiguously-correct
    // trade for a first hardware-verified cut: stall once per frame instead).
    dkQueueWaitIdle(g_queue);

    g_parity ^= 1;
    g_game_pending = g_hud_pending = g_overlay_pending = false;
}

void gpu_video_exit(void) {
    if (!g_ready) return;
    g_ready = false;
    teardown();
}
