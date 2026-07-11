// Experimental: offloads the AI Upscale (ESPCN) compute to a laptop over the
// local network instead of running it on the Switch's own GPU — see the
// research memo. The Switch's own compute-shader implementation measured
// ~312ms/frame even after two optimization passes; a 2019 MacBook Pro
// measured ~7ms/frame steady-state for the exact same network.
//
// Protocol v2 (UDP): authenticated by a shared pairing code (never sent over
// the wire itself — proven via HMAC-SHA256 challenge/response), after which
// frames flow in PLAINTEXT datagrams tagged with a per-session token.
// Requests usually fit one ~57KB datagram (link-bench-verified: ~40 IP
// fragments, zero loss on the direct cable); responses arrive as 16KB
// chunks. Both directions use XOR-diff + zstd encoding against a reference
// frame the receiver has explicitly confirmed holding (ACK ids ride in every
// frame header), with automatic keyframe fallbacks on scene cuts (dense
// diff), reference misses, and reconnects — so a lost datagram can cost one
// frame but can never silently corrupt the stream. See
// native/tools/net_upscale_server.py for the matching laptop-side half and
// the full wire format — the two must be kept in lockstep.
//
// Security model (deliberately weaker than v1's per-frame AEAD — a measured
// tradeoff): the challenge-response gate still means nobody can use the
// laptop's GPU without the pairing code, but frame data itself is
// unencrypted and unauthenticated — someone on the same network segment who
// can sniff the session token could inject forged frames. On the intended
// link (a point-to-point Ethernet cable) there is nobody else on the wire,
// and the data is SNES video frames. v1's ChaCha20-Poly1305 sealing was
// dropped because its per-frame decrypt cost on the Switch's CPU was a
// measured, real slice of the round trip.
//
// Fails soft, always: a bad host string, wrong pairing code, unreachable
// server, or a mid-game disconnect all just mean no new result arrives — the
// caller (gpu_video.c) falls back to the local render path, or keeps showing
// the last result it had, and gameplay is never permanently stuck waiting on
// the network. Note this no longer means "never blocks at all", though: when
// Network Upscale is active, net_upscale_submit_and_wait() deliberately DOES
// block the calling thread (bounded by its own timeout_ms — see its comment)
// so each displayed frame's upscale actually corresponds to that frame,
// rather than to whichever previous frame's result happened to be latched.
// net_upscale_submit_frame() remains the non-blocking, fire-and-forget entry
// point, still used wherever true "never block" behavior is wanted.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Parses "host:port" from `host_port` and starts a background thread that
// connects, performs the pairing handshake, and thereafter pumps frames.
// Returns immediately (never blocks on the network) — connection progress is
// polled via net_upscale_connected(). Returns false only for malformed
// input (unparseable host:port, or pairing_code too long); a failed/slow
// *connection* is not a false-return case, since that's expected and handled
// by the background thread's own retry loop.
//
// `compression`: gates the whole diff+zstd encoding pipeline, both
// directions (off = raw full frames both ways — the clean A/B baseline).
// Sent to the server in the AUTH datagram each handshake, so it's
// negotiated per session, not per frame.
bool net_upscale_init(const char *host_port, const char *pairing_code, bool compression);

// Updates the compression preference, independent of net_upscale_init
// (which only ever runs once per app session — see its call site's
// !g_net_initialized guard in main.c). If a live session negotiated a
// different value, that session is torn down and re-handshaken
// automatically, so the toggle takes real effect within about a second —
// compression is agreed at handshake, and without the forced reconnect the
// toggle would half-apply (requests reacting immediately, responses keeping
// the old setting — a real mixed state hit on hardware).
void net_upscale_set_compression(bool compression);

// Advisory hint: which way the player is currently pushing (each of dir_x /
// dir_y in {-1, 0, 1}). Directional input directly predicts camera-scroll
// direction, so the network thread uses it to seed candidate shifts for
// request-side motion compensation — current the instant a walk starts,
// stops, or turns, where the other candidates (the server's last reported
// shift) lag a frame. Purely advisory: every candidate is TESTED against the
// actual frame content before use, so a wrong (or missing) hint costs
// nothing but a slightly bigger payload. Call once per game frame, before
// net_upscale_submit_frame/submit_and_wait.
void net_upscale_hint_input(int dir_x, int dir_y);

// Submits this frame's luma plane (tightly packed, w*h bytes, w/h <= 512) for
// network upscaling. Non-blocking: copies into a single pending-send slot.
// If the previous submission hasn't been picked up by the network thread
// yet, this one replaces it — always send the newest frame, never queue
// stale ones behind a slow or momentarily-stalled connection.
void net_upscale_submit_frame(const uint8_t *luma, unsigned w, unsigned h);

// Like net_upscale_submit_frame, but blocks the calling thread until this
// exact frame's result is ready (returning it via the optional out params,
// each of which may be NULL) or timeout_ms elapses, whichever comes first —
// never longer, even against a fully dead connection. Intended to be called
// from inside retro_run() (via video_refresh) when Network Upscale is on, so
// the game's frame completion is actually gated on that frame's AI-upscaled
// result, instead of treating the network round trip as background work the
// game never waits for. Returns false on timeout/any failure — the caller
// should keep showing whatever it already had (net_upscale_get_result()
// still returns the last successful result either way) rather than treat
// this as fatal.
bool net_upscale_submit_and_wait(const uint8_t *luma, unsigned w, unsigned h,
                                  unsigned timeout_ms,
                                  const uint8_t **out_luma, unsigned *out_w, unsigned *out_h);

// Returns the most recently received upscaled luma plane (w*3 x h*3, tightly
// packed) via *out_luma/*out_w/*out_h, or false if none has arrived yet (not
// connected, or the first frame hasn't round-tripped). The returned pointer
// is only valid until the next call to this function or to
// net_upscale_exit() — copy out anything the caller needs to keep.
bool net_upscale_get_result(const uint8_t **out_luma, unsigned *out_w, unsigned *out_h);

// True once the pairing handshake has succeeded and the session is live.
bool net_upscale_connected(void);

// Wall-clock time (microseconds) of the last full send+recv round trip —
// the HUD's equivalent of gpu_video_get_ai_upscale_us() for the local GPU
// path. 0 if no round trip has completed yet.
unsigned net_upscale_get_last_rtt_us(void);

// Increments once per new result landed. net_upscale_get_result() re-returns
// the same latched result on every poll, so callers that need to distinguish
// "a genuinely new round trip just completed" from "still showing the same
// one as last poll" (e.g. HUD timing averages) should only act when this
// value has changed since their last observation.
unsigned net_upscale_get_result_generation(void);

// Stops the background threads and closes the socket. Safe to call even if
// net_upscale_init was never called, or failed.
void net_upscale_exit(void);
