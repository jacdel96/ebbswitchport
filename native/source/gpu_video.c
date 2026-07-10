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

#define CODE_MEM_SIZE (64 * 1024)
#define CMD_MEM_SIZE  (64 * 1024)

// Image descriptor slots (indices into the bound image descriptor set).
enum { IMG_GAME = 0, IMG_HUD = 1, IMG_OVERLAY = 2, IMG_COUNT = 3 };
#define SAMPLER_SLOT 0

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

static DkMemBlock g_descMemBlock;   // 3 image descriptors + 1 sampler descriptor
#define DESC_IMG_OFFSET(slot) ((slot) * DK_IMAGE_DESCRIPTOR_ALIGNMENT)
#define DESC_SAMPLER_OFFSET   256

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
    }
    if (g_overlayImgMem) dkMemBlockDestroy(g_overlayImgMem);
    if (g_hudImgMem) dkMemBlockDestroy(g_hudImgMem);
    if (g_gameImgMem) dkMemBlockDestroy(g_gameImgMem);
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
    memset(g_gameScratch, 0, sizeof(g_gameScratch));
    memset(g_hudScratch, 0, sizeof(g_hudScratch));
    memset(g_overlayScratch, 0, sizeof(g_overlayScratch));
    g_descMemBlock = NULL;
    g_cmdbufMemBlock = NULL;
    g_cmdbuf = NULL;
}

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
    g_descMemBlock = NULL;
    g_cmdbufMemBlock = NULL;
    g_cmdbuf = NULL;
    g_game_w = g_game_h = 0;
    g_hud_visible = g_overlay_visible = false;
    g_crt_enabled = false;
    g_game_pending = g_hud_pending = g_overlay_pending = false;
    g_parity = 0;
    g_win = win;
    resolution_for_mode(&g_fb_w, &g_fb_h);  // start at whatever mode we're already in
    compute_dst_layout();

    DkDeviceMaker devMaker;
    dkDeviceMakerDefaults(&devMaker);
    g_device = dkDeviceCreate(&devMaker);
    if (!g_device) return false;

    DkQueueMaker qMaker;
    dkQueueMakerDefaults(&qMaker, g_device);
    qMaker.flags = DkQueueFlags_Graphics;
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
    for (int i = 0; i < N_PARITY; i++) {
        g_gameScratch[i] = make_memblock(gameScratchSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        g_hudScratch[i] = make_memblock(hudScratchSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        g_overlayScratch[i] = make_memblock(overlayScratchSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        if (!g_gameScratch[i] || !g_hudScratch[i] || !g_overlayScratch[i]) { teardown(); return false; }
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

void gpu_video_present(void) {
    if (!g_ready) return;

    // Cheap to poll every frame; only actually resizes on a real dock/handheld
    // change. Safe to touch swapchain resources here with no extra wait: the
    // GPU is guaranteed idle at this point, either from init's own wait or
    // from the trailing dkQueueWaitIdle at the end of every previous call.
    unsigned want_w, want_h;
    resolution_for_mode(&want_w, &want_h);
    if (want_w != g_fb_w || want_h != g_fb_h) resize_swapchain(want_w, want_h);

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
    bool any_pending = g_game_pending || g_hud_pending || g_overlay_pending;
    if (g_game_pending) {
        DkCopyBuf src = { dkMemBlockGetGpuAddr(g_gameScratch[g_parity]), 0, 0 };
        DkImageView view;
        dkImageViewDefaults(&view, &g_gameImage);
        DkImageRect rect = { 0, 0, 0, g_pending_game_w, g_pending_game_h, 1 };
        dkCmdBufCopyBufferToImage(g_cmdbuf, &src, &view, &rect, 0);
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
        draw_textured_quad(IMG_GAME, g_dst_x0, g_dst_y0, (int)g_dst_w, (int)g_dst_h);
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
