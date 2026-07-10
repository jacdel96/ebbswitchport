// Experimental: offloads the AI Upscale (ESPCN) compute to a laptop over the
// local network instead of running it on the Switch's own GPU — see the
// research memo. The Switch's own compute-shader implementation measured
// ~312ms/frame even after two optimization passes; a 2019 MacBook Pro
// measured ~7ms/frame steady-state for the exact same network.
//
// Security: the connection is authenticated by a shared pairing code (never
// sent over the wire itself — proven via HMAC-SHA256 challenge/response),
// then every subsequent message is ChaCha20-Poly1305 sealed under a fresh
// per-session key (HKDF-derived from the pairing code and the connection's
// random challenge), so a captured handshake can't be replayed later. See
// native/tools/net_upscale_server.py for the matching laptop-side half of
// this protocol — the two must be kept in lockstep.
//
// Fails soft, always: a bad host string, wrong pairing code, unreachable
// server, or a mid-game disconnect all just mean net_upscale_get_result()
// returns false — gameplay is never blocked waiting on the network, and the
// caller (gpu_video.c) falls back to the local render path when there's no
// result available.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Parses "host:port" from `host_port` and starts a background thread that
// connects, performs the pairing handshake, and thereafter pumps frames.
// Returns immediately (never blocks on the network) — connection progress is
// polled via net_upscale_connected(). Returns false only for malformed
// input (unparseable host:port, or pairing_code too long); a failed/slow
// *connection* is not a false-return case, since that's expected on real
// WiFi and handled by the background thread's own retry loop.
bool net_upscale_init(const char *host_port, const char *pairing_code);

// Submits this frame's luma plane (tightly packed, w*h bytes, w/h <= 512) for
// network upscaling. Non-blocking: copies into a single pending-send slot.
// If the previous submission hasn't been picked up by the network thread
// yet, this one replaces it — always send the newest frame, never queue
// stale ones behind a slow or momentarily-stalled connection.
void net_upscale_submit_frame(const uint8_t *luma, unsigned w, unsigned h);

// Returns the most recently received upscaled luma plane (w*3 x h*3, tightly
// packed) via *out_luma/*out_w/*out_h, or false if none has arrived yet (not
// connected, or the first frame hasn't round-tripped). The returned pointer
// is only valid until the next call to this function or to
// net_upscale_exit() — copy out anything the caller needs to keep.
bool net_upscale_get_result(const uint8_t **out_luma, unsigned *out_w, unsigned *out_h);

// True once the pairing handshake has succeeded and the connection is live.
bool net_upscale_connected(void);

// Stops the background thread and closes the socket. Safe to call even if
// net_upscale_init was never called, or failed.
void net_upscale_exit(void);
