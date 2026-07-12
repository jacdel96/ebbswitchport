#!/usr/bin/env python3
"""Laptop-side ESPCN inference server for ebbswitchport's "Network AI Upscale".

Offloads the expensive 3-layer super-resolution network to a real GPU/CPU
instead of the Switch's Tegra X1 (see the research memo: the Switch's own
compute-shader implementation of this same network measured ~312ms/frame —
this laptop measures ~7ms/frame steady-state for the exact same network).

Protocol v2 (UDP, port --port; all integers little-endian; every datagram
starts with the 4-byte magic "EBU2" followed by a 1-byte type):

  Handshake (client retries each step; server keeps only a short-lived
  pending-challenge entry per address until AUTH lands):
    HELLO     [magic][0x01]
    CHALLENGE [magic][0x02][16B random challenge]
    AUTH      [magic][0x03][32B HMAC-SHA256(psk_key, challenge)][1B compression]
    ACCEPT    [magic][0x04][8B session token][1B compression]
    REJECT    [magic][0x05]
    PING      [magic][0x06][8B token]          (keeps an idle session alive)
  The pairing code itself never crosses the wire — challenge-response, same
  as v1. psk_key derivation (HKDF) is also unchanged, so saved pairing codes
  keep working.

  Frames (both directions, chunked; 46-byte header):
    [magic 4][type u8][flags u8][chunk_idx u16][chunk_count u16]
    [w u16][h u16][frame_id u32][total_len u32][uncomp_len u32][offset u32]
    [ref_id u32][peer_id u32][token u64][payload]
    type 0x10 = request chunk, 0x11 = response chunk
    flags bit0 COMP     payload is zstd-compressed
    flags bit1 DIFF     payload (after decompression) is an XOR plane
                        against frame ref_id, not absolute pixels
    flags bit2 NEED_KEY (requests only) client lost response-diff sync —
                        reply to this frame with a keyframe
    flags bit3 NACK     (responses only) request ref_id wasn't available
                        server-side; no payload; client falls back to raw
    peer_id: in requests, "the output frame_id the client currently holds"
             (what response diffs may reference). In responses, "the newest
             input frame_id the server holds" (the ACK that authorizes the
             client to send request diffs against it).

  Diff encoding correctness over a lossy transport: each side only ever
  diffs against a frame the OTHER side has explicitly declared holding
  (peer_id), never just "the previous frame" — so a lost datagram can
  degrade one frame, never silently corrupt the stream. First frame each
  direction is a full keyframe; scene cuts fall back to keyframes on their
  own (a dense XOR plane fails the client's sparsity check / compresses no
  better than a full frame).

Security model (deliberately weaker than v1 — a measured tradeoff):
  v1 sealed every frame with ChaCha20-Poly1305; profiling on real hardware
  showed the per-frame decrypt cost on the Switch's CPU was a real slice of
  the round trip. v2 keeps the challenge-response gate (nobody can use your
  GPU without the pairing code) but frame DATA is plaintext and
  unauthenticated: anyone on the same segment who can sniff the session
  token could inject forged frames. On the intended link — a point-to-point
  Ethernet cable between the console and this machine — there is nobody
  else on the wire, and the data is SNES video frames. Do not run this
  across a network you don't trust.

Usage:
    python net_upscale_server.py --weights /path/to/espcn_x3.bin \\
        --pairing-code "correct horse battery staple"
"""
import argparse
import hashlib
import hmac
import os
import queue
import random
import socket
import struct
import threading
import time
import zlib
import zstandard as zstd

import numpy as np
import torch
import torch.nn as nn
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes

WEIGHTS_MAGIC = b"ESP1"

NET_MAGIC = b"EBU2"
T_HELLO, T_CHALLENGE, T_AUTH, T_ACCEPT, T_REJECT, T_PING = 1, 2, 3, 4, 5, 6
T_FRAME_REQ, T_FRAME_RESP = 0x10, 0x11
CHALLENGE_LEN = 16
HMAC_LEN = 32

FRAME_HDR = struct.Struct("<4sBBHHHHIIIIIIhhQ")
# (magic, type, flags, chunk_idx, chunk_count, w, h, frame_id, total_len,
#  uncomp_len, offset, ref_id, peer_id, mc_dx, mc_dy, token) = 50 bytes
FLAG_COMP = 1
FLAG_DIFF = 2
FLAG_NEED_KEY = 4
FLAG_NACK = 8
FLAG_MC = 16   # responses only: the diff was taken against the reference
               # SHIFTED by (mc_dx, mc_dy) output-space pixels (zero-filled
               # at the revealed edges) — motion compensation for camera
               # scroll, which otherwise makes XOR diffs dense exactly when
               # frames are most frequent

MC_MAX_INPUT_SHIFT = 32   # input-space search bound; scrolls are a few px/frame

RESP_CHUNK = 16384   # response chunk payload size — bench showed identical
                      # throughput to 60KB chunks, and smaller chunks put less
                      # pressure on the Switch's 256KB UDP receive buffer
MAX_W = MAX_H = 512
RING = 4             # recent inputs/outputs retained per session for diffs —
                      # covers pipelining depth 2 with slack

SESSION_IDLE_S = 60.0
PENDING_CHALLENGE_TTL_S = 30.0
PARTIAL_TTL_S = 2.0


class ESPCN(nn.Module):
    """Matches native/source/shaders/espcn{1,2,3}_comp.glsl — same topology,
    same tanh placement, same lack of activation on layer 3.  Channel widths
    and kernel sizes default to the original 64/32 5-3-3 but are derived from
    the loaded ESP1 array shapes by TorchInference (mirroring what
    MPSGraphInference already does), so slimmer distilled checkpoints from
    native/tools/distill/train_student.py load through the same class."""

    def __init__(self, c1=64, c2=32, k1=5, k2=3, k3=3):
        super().__init__()
        self.conv1 = nn.Conv2d(1, c1, k1, padding=k1 // 2)
        self.conv2 = nn.Conv2d(c1, c2, k2, padding=k2 // 2)
        self.conv3 = nn.Conv2d(c2, 9, k3, padding=k3 // 2)
        self.shuffle = nn.PixelShuffle(3)
        self.tanh = nn.Tanh()

    def forward(self, x):
        x = self.tanh(self.conv1(x))
        x = self.tanh(self.conv2(x))
        x = self.conv3(x)
        return self.shuffle(x)


def read_weight_arrays(bin_path):
    """Loads native/tools/package_weights.py's binary format directly —
    struct/numpy only, no torch.load/pickle involved for this file."""
    with open(bin_path, "rb") as f:
        magic = f.read(4)
        if magic != WEIGHTS_MAGIC:
            raise ValueError(f"bad magic {magic!r}, expected {WEIGHTS_MAGIC!r}")
        (scale,) = struct.unpack("<I", f.read(4))
        if scale != 3:
            raise ValueError(f"unsupported scale {scale}, only x3 is wired up")
        c1 = struct.unpack("<4I", f.read(16))
        c2 = struct.unpack("<4I", f.read(16))
        c3 = struct.unpack("<4I", f.read(16))

        def read_arr(shape):
            n = 1
            for s in shape:
                n *= s
            return np.frombuffer(f.read(n * 4), dtype="<f4").reshape(shape).copy()

        w1, b1 = read_arr(c1), read_arr((c1[0],))
        w2, b2 = read_arr(c2), read_arr((c2[0],))
        w3, b3 = read_arr(c3), read_arr((c3[0],))
        trailing = f.read()
        if trailing:
            raise ValueError(f"{len(trailing)} unexpected trailing bytes")
    return w1, b1, w2, b2, w3, b3


def load_weights(model, bin_path):
    w1, b1, w2, b2, w3, b3 = read_weight_arrays(bin_path)
    # Assigning straight from torch.from_numpy() would silently reset these
    # parameters to CPU tensors even if the model was already .to(device)'d
    # (a real bug hit while testing this) — move each one explicitly.
    device = next(model.parameters()).device
    model.conv1.weight.data = torch.from_numpy(w1).to(device)
    model.conv1.bias.data = torch.from_numpy(b1).to(device)
    model.conv2.weight.data = torch.from_numpy(w2).to(device)
    model.conv2.bias.data = torch.from_numpy(b2).to(device)
    model.conv3.weight.data = torch.from_numpy(w3).to(device)
    model.conv3.bias.data = torch.from_numpy(b3).to(device)


# ---- inference backends -------------------------------------------------------
# Two implementations of the same contract:
#   .name             human-readable, for the startup log
#   .wants_keepalive  whether the idle-GPU wake-up tax applies (see gpu_worker)
#   .infer(luma, h, w) -> np.uint8 array shaped (1, 1, 3h, 3w)
#   .keepalive(shape) run a dummy inference at (h, w) to keep the GPU warm
#
# MPSGraph-direct is the default where available: same Apple conv kernels
# PyTorch/MPS uses underneath, but precompiled to a fixed-shape executable
# with preallocated zero-copy MTLBuffers — measured 3.7ms/frame vs PyTorch
# eager's 5.5ms (the ~2ms delta is pure framework dispatch, not compute).
# Output was verified bit-exact against the PyTorch reference (max 1 LSB).


class TorchInference:
    def __init__(self, weights_path):
        self.device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
        w1, _, w2, _, w3, _ = read_weight_arrays(weights_path)
        self.model = ESPCN(c1=w1.shape[0], c2=w2.shape[0], k1=w1.shape[2],
                           k2=w2.shape[2], k3=w3.shape[2]).to(self.device).eval()
        load_weights(self.model, weights_path)
        self.bufs = {}   # (h, w) -> persistent GPU input tensor (worker-thread-only)
        self.wants_keepalive = self.device.type == "mps"
        self.name = f"torch-{self.device.type}"

    def infer(self, luma, h, w):
        cpu_view = torch.from_numpy(luma.reshape(1, 1, h, w).astype(np.float32) / 255.0)
        buf = self.bufs.get((h, w))
        if buf is None:
            buf = torch.empty((1, 1, h, w), dtype=torch.float32, device=self.device)
            self.bufs[(h, w)] = buf
        with torch.no_grad():
            buf.copy_(cpu_view)
            y = self.model(buf)
            if self.device.type == "mps":
                torch.mps.synchronize()
            return (y.clamp(0, 1) * 255.0).round().to(torch.uint8).cpu().numpy()

    def keepalive(self, shape):
        h, w = shape
        buf = self.bufs.get(("ka", h, w))
        if buf is None:
            buf = torch.zeros((1, 1, h, w), dtype=torch.float32, device=self.device)
            self.bufs[("ka", h, w)] = buf
        with torch.no_grad():
            _ = self.model(buf)
            if self.device.type == "mps":
                torch.mps.synchronize()


class MPSGraphInference:
    _MPS_F32 = 0x10000000 | 32

    def __init__(self, weights_path):
        # Imports live here so machines without pyobjc (or without Metal)
        # cleanly fall back to TorchInference via make_inference_backend.
        import Metal
        import MetalPerformanceShadersGraph as G
        from Foundation import NSData, NSNumber, NSArray
        self._G, self._NSData, self._NSNumber, self._NSArray = G, NSData, NSNumber, NSArray

        self.weights = read_weight_arrays(weights_path)
        self.device = Metal.MTLCreateSystemDefaultDevice()
        if self.device is None:
            raise RuntimeError("no Metal device")
        self.queue = self.device.newCommandQueue()
        self.gdev = G.MPSGraphDevice.deviceWithMTLDevice_(self.device)
        self.cache = {}  # (h, w) -> dict(exe, in_buf, out_buf, in_td, out_td, sizes)
        self.name = f"mpsgraph ({self.device.name()})"
        self.wants_keepalive = True
        # Warm the shapes we know we'll need: shape compilation costs ~0.5s
        # a piece (measured), which must never land on a live frame.
        self._entry(224, 256)
        self._entry(8, 8)

    def _shape(self, *dims):
        return self._NSArray.arrayWithArray_(
            [self._NSNumber.numberWithInt_(d) for d in dims])

    def _entry(self, h, w):
        e = self.cache.get((h, w))
        if e is not None:
            return e
        G, NSData = self._G, self._NSData
        w1, b1, w2, b2, w3, b3 = self.weights
        graph = G.MPSGraph.alloc().init()
        inp = graph.placeholderWithShape_dataType_name_(
            self._shape(1, 1, h, w), self._MPS_F32, "input")

        def const(np_arr, *dims):
            arr = np.ascontiguousarray(np_arr, dtype=np.float32)
            data = NSData.dataWithBytes_length_(arr.tobytes(), arr.nbytes)
            return graph.constantWithData_shape_dataType_(
                data, self._shape(*dims), self._MPS_F32)

        def conv(x, wgt, bias, name):
            # Everything is derived from the loaded array shapes, so any
            # ESP1 file (including slim distilled students) compiles as-is.
            oc, ic, k = wgt.shape[0], wgt.shape[1], wgt.shape[2]
            pad = k // 2
            desc = G.MPSGraphConvolution2DOpDescriptor.descriptorWithStrideInX_strideInY_dilationRateInX_dilationRateInY_groups_paddingLeft_paddingRight_paddingTop_paddingBottom_paddingStyle_dataLayout_weightsLayout_(
                1, 1, 1, 1, 1, pad, pad, pad, pad,
                0,   # explicit padding
                0,   # data layout NCHW
                2)   # weights layout OIHW
            y = graph.convolution2DWithSourceTensor_weightsTensor_descriptor_name_(
                x, const(wgt, oc, ic, k, k), desc, name)
            return graph.additionWithPrimaryTensor_secondaryTensor_name_(
                y, const(bias.reshape(1, oc, 1, 1), 1, oc, 1, 1), name + "_b")

        x = conv(inp, w1, b1, "c1")
        x = graph.tanhWithTensor_name_(x, "t1")
        x = conv(x, w2, b2, "c2")
        x = graph.tanhWithTensor_name_(x, "t2")
        x = conv(x, w3, b3, "c3")
        # PixelShuffle(3) == depthToSpace with pixel-shuffle ordering (NCHW).
        x = graph.depthToSpace2DTensor_widthAxis_heightAxis_depthAxis_blockSize_usePixelShuffleOrder_name_(
            x, 3, 2, 1, 3, True, "ps")
        # Bake the postprocess (clamp/scale/round) into the graph so the CPU
        # side is just a float->u8 cast.
        zero = graph.constantWithScalar_dataType_(0.0, self._MPS_F32)
        one = graph.constantWithScalar_dataType_(1.0, self._MPS_F32)
        v255 = graph.constantWithScalar_dataType_(255.0, self._MPS_F32)
        x = graph.clampWithTensor_minValueTensor_maxValueTensor_name_(x, zero, one, "clamp")
        x = graph.multiplicationWithPrimaryTensor_secondaryTensor_name_(x, v255, "scale")
        out = graph.roundWithTensor_name_(x, "round")

        shaped = G.MPSGraphShapedType.alloc().initWithShape_dataType_(
            self._shape(1, 1, h, w), self._MPS_F32)
        exe = graph.compileWithDevice_feeds_targetTensors_targetOperations_compilationDescriptor_(
            self.gdev, {inp: shaped}, [out], None, None)

        oh, ow = h * 3, w * 3
        in_bytes, out_bytes = h * w * 4, oh * ow * 4
        in_buf = self.device.newBufferWithLength_options_(in_bytes, 0)
        out_buf = self.device.newBufferWithLength_options_(out_bytes, 0)
        e = {
            "exe": exe,
            "in_buf": in_buf, "out_buf": out_buf,
            "in_td": G.MPSGraphTensorData.alloc().initWithMTLBuffer_shape_dataType_(
                in_buf, self._shape(1, 1, h, w), self._MPS_F32),
            "out_td": G.MPSGraphTensorData.alloc().initWithMTLBuffer_shape_dataType_(
                out_buf, self._shape(1, 1, oh, ow), self._MPS_F32),
            "in_bytes": in_bytes, "out_bytes": out_bytes,
            "oh": oh, "ow": ow,
        }
        self.cache[(h, w)] = e
        self._run(e)  # first run finishes any lazy specialization off-frame
        return e

    def _run(self, e):
        e["exe"].runWithMTLCommandQueue_inputsArray_resultsArray_executionDescriptor_(
            self.queue, [e["in_td"]], [e["out_td"]], None)

    def infer(self, luma, h, w):
        e = self._entry(h, w)
        x = luma.reshape(-1).astype(np.float32) / 255.0
        e["in_buf"].contents().as_buffer(e["in_bytes"])[:] = x.tobytes()
        self._run(e)
        out = np.frombuffer(e["out_buf"].contents().as_buffer(e["out_bytes"]),
                            dtype=np.float32)
        return out.astype(np.uint8).reshape(1, 1, e["oh"], e["ow"])

    def keepalive(self, shape):
        e = self.cache.get(shape)
        if e is None:
            # Never compile on the keep-alive path — a ~0.5s stall is worse
            # than a cold GPU. Fall back to the tiny warmed shape.
            e = self.cache.get((8, 8))
            if e is None:
                return
        self._run(e)


def make_inference_backend(weights_path):
    pref = os.environ.get("INFER_BACKEND", "auto")
    if pref in ("auto", "mpsgraph"):
        try:
            return MPSGraphInference(weights_path)
        except Exception as e:
            if pref == "mpsgraph":
                raise
            print(f"[i] mpsgraph backend unavailable ({e}); falling back to torch")
    return TorchInference(weights_path)


def derive_psk_key(pairing_code: str) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(), length=32,
        salt=b"ebbswitchport-espcn-psk", info=b"psk",
    ).derive(pairing_code.encode("utf-8"))


# Single-writer GPU access: exactly one thread (gpu_worker, below) ever
# calls into the model. MPS is NOT thread-safe for concurrent model calls
# from two threads sharing the same underlying command queue — confirmed
# the hard way, twice: a lock scoped to just model()+synchronize() still
# crashed the server ("commit an already committed command buffer"),
# because postprocess (clamp/round/cast/.cpu()) also touches the GPU and
# was slipping through outside the lock's scope. Rather than keep
# widening a lock and hoping every GPU-touching line is covered, all real
# requests go through this one queue to a single consuming thread — no
# lock needed, because there's only ever one thread that can touch the
# GPU in the first place.
_gpu_queue = queue.Queue()
_resp_queue = queue.Queue()


def gpu_worker(backend):
    """The only thread that ever calls into the GPU. Pulls real work off the
    queue when there is any; when the queue's empty for long enough that
    the GPU would otherwise idle down (pytorch/pytorch#124056 — confirmed
    open, affects MPS and CUDA both, ~5ms wake-up cost measured here after
    a ~30-45ms gap, matching real network-paced request cadence), fires a
    dummy inference instead to keep it from dropping into that state. The
    dummy is sized to match the last real frame seen (falling back to the
    SNES's native 256x224 before the first real request arrives) rather
    than some arbitrary tiny shape — a trivially small op can dispatch
    different kernels/tile sizes than the real workload, so it wouldn't
    reliably warm the same GPU codepath the next real request needs.
    Ping cadence is adaptive, not a fixed interval: it tracks a rolling
    average of the actual gap between real requests and pings at roughly
    half that gap, clamped to [2ms, 20ms] — 1ms-vs-8ms testing showed no
    latency benefit below that band, and a long idle stretch must not stop
    pinging altogether. A small jitter term keeps the polling from settling
    into a perfectly periodic cadence that could alias with the request
    cadence or another fixed-period system timer.
    """
    last_shape = (224, 256)  # SNES native resolution — best guess before any real frame lands
    # Keep-alive tuning, settled empirically at real gameplay cadence:
    #   The tension: a frame-sized dummy warms the GPU best (7.1ms forward vs
    #   tiny's ~9-10ms vs ~12ms stone cold) because wake-up cost tracks how
    #   recently REAL-sized work ran — but it costs as much as a real
    #   inference, so one that fires late in the gap collides with the next
    #   request (measured 25-55% collision rate, 4-5ms avg queue wait).
    #   Tiny (8x8) dummies never meaningfully collide (~1-2ms worst case) but
    #   warm less. And firing full dummies only EARLY in the gap avoids
    #   collisions while losing the entire warming benefit — activity
    #   shortly BEFORE the next request is what matters, not after the last.
    #   Hence "hybrid": frame-sized dummies while enough gap remains for one
    #   to finish safely before the next request is due, tiny pings for the
    #   final stretch. Warmth of full, collision cost of tiny.
    MIN_PING_INTERVAL = 0.002
    MAX_PING_INTERVAL = 0.006
    EMA_ALPHA = 0.2
    FULL_DUMMY_SAFETY_S = 0.010  # ~7ms dummy + ~3ms arrival jitter
    avg_gap = 0.021  # seeded at the measured wired-link request cadence
    last_job_time = None
    last_keepalive_end = 0.0
    keepalive_mode = os.environ.get("KEEPALIVE_MODE", "hybrid")  # hybrid|tiny|full|off, for A/B testing
    interval_override = float(os.environ.get("KEEPALIVE_INTERVAL_MS", "0")) / 1000.0
    while True:
        ping_interval = min(MAX_PING_INTERVAL, max(MIN_PING_INTERVAL, avg_gap / 4))
        if interval_override > 0:
            ping_interval = interval_override
        try:
            job = _gpu_queue.get(timeout=ping_interval + random.uniform(0.0, ping_interval * 0.15))
        except queue.Empty:
            # Keep-alive pings only make sense on GPUs that drop to a lower
            # power state when idle (pytorch/pytorch#124056 — applies to raw
            # Metal/MPSGraph identically, verified with GPU timestamps);
            # plain CPU has no such penalty, so don't burn cycles for nothing.
            if backend.wants_keepalive and keepalive_mode != "off":
                if keepalive_mode == "tiny":
                    shape = (8, 8)
                elif keepalive_mode == "hybrid" and last_job_time is not None:
                    elapsed = time.perf_counter() - last_job_time
                    in_safe_window = elapsed < avg_gap - FULL_DUMMY_SAFETY_S
                    shape = last_shape if in_safe_window else (8, 8)
                else:
                    shape = last_shape
                backend.keepalive(shape)
                last_keepalive_end = time.perf_counter()
            continue

        luma, h, w, meta = job

        now = time.perf_counter()
        meta["t_dequeue"] = now
        # A keep-alive dummy that was still running when this request was
        # enqueued made the request wait for it — a real collision cost worth
        # counting separately from genuine GPU time.
        meta["ka_collision"] = last_keepalive_end > meta.get("t_enqueue", 0.0)
        if last_job_time is not None:
            gap = now - last_job_time
            avg_gap = EMA_ALPHA * gap + (1 - EMA_ALPHA) * avg_gap
        last_job_time = now

        last_shape = (h, w)
        out = backend.infer(luma, h, w)
        meta["t_infer"] = time.perf_counter()
        _resp_queue.put((meta, out))
        _gpu_queue.task_done()


def estimate_shift(ref_img, cur_img, max_shift=MC_MAX_INPUT_SHIFT):
    """Global-translation estimate between two same-shape uint8 planes, via
    phase correlation (FFT of the normalized cross-power spectrum — the
    standard trick; sub-ms in numpy at 256x224). Returns (dx, dy) meaning
    cur(y, x) ~= ref(y - dy, x - dx), or (0, 0) when there's no shift worth
    using. A sampled SAD check guards against garbage peaks on scene cuts:
    the shift must actually explain the frame change much better than no
    shift, or we fall back to the plain diff."""
    a = ref_img.astype(np.float32)
    b = cur_img.astype(np.float32)
    fa = np.fft.rfft2(a)
    fb = np.fft.rfft2(b)
    cross = fb * np.conj(fa)
    mag = np.abs(cross)
    mag[mag < 1e-9] = 1e-9
    corr = np.fft.irfft2(cross / mag, s=a.shape)
    dy, dx = np.unravel_index(np.argmax(corr), corr.shape)
    if dy > a.shape[0] // 2:
        dy -= a.shape[0]
    if dx > a.shape[1] // 2:
        dx -= a.shape[1]
    if (dx == 0 and dy == 0) or abs(dx) > max_shift or abs(dy) > max_shift:
        return 0, 0
    # Sampled verification, margins > max_shift so np.roll's wrapped strips
    # never pollute the samples.
    m = max_shift * 2
    ys, xs = slice(m, a.shape[0] - m, 7), slice(m, a.shape[1] - m, 7)
    cur_s = b[ys, xs]
    sad_shift = np.abs(cur_s - np.roll(a, (dy, dx), axis=(0, 1))[ys, xs]).mean()
    sad_plain = np.abs(cur_s - a[ys, xs]).mean()
    if sad_shift * 2.0 > sad_plain:
        return 0, 0
    return int(dx), int(dy)


def shifted_plane(ref_flat, oh, ow, dy, dx):
    """ref shifted by (dy, dx) with zero fill at the revealed edges —
    shifted(y, x) = ref(y - dy, x - dx)."""
    ref = ref_flat.reshape(oh, ow)
    out = np.zeros_like(ref)
    out[max(0, dy):oh - max(0, -dy), max(0, dx):ow - max(0, -dx)] = \
        ref[max(0, -dy):oh - max(0, dy), max(0, -dx):ow - max(0, dx)]
    return out.reshape(-1)


def make_session(addr, token, compression):
    return {
        "addr": addr,
        "label": f"{addr[0]}:{addr[1]}",
        "token": token,
        "compression": compression,
        "last_seen": time.monotonic(),
        "partials": {},          # frame_id -> partial reassembly
        "inputs": {},            # frame_id -> (np flat uint8, w, h) — last RING decoded inputs
        "outputs": {},           # frame_id -> (np flat uint8, ow, oh) — last RING sent outputs
        "latest_input_id": 0,
        # window stats (printed by the responder thread)
        "stats": {
            "infer": [], "total": [], "resp_kb": [], "diff_frames": 0,
            "key_frames": 0, "req_diff_frames": 0, "req_raw_frames": 0,
            # fine-grained split of the old "inference" bucket, for the
            # inflation diagnosis: decode (request decompress/XOR/convert in
            # the recv thread), qwait (sitting in the GPU queue), gpu (copy +
            # forward + sync + postprocess), plus responder-side xor/compress
            # and how many frames collided with a keep-alive dummy.
            "decode": [], "qwait": [], "gpu": [], "encode": [],
            "ka_collisions": 0,
        },
    }


def ring_put(ring, key, value):
    ring[key] = value
    while len(ring) > RING:
        del ring[min(ring.keys())]


def send_chunked(sock, addr, frame_type, flags, w, h, frame_id,
                 payload, uncomp_len, ref_id, peer_id, token, mc_dx=0, mc_dy=0):
    total = len(payload)
    count = max(1, (total + RESP_CHUNK - 1) // RESP_CHUNK)
    off = 0
    for i in range(count):
        part = payload[off:off + RESP_CHUNK]
        hdr = FRAME_HDR.pack(NET_MAGIC, frame_type, flags, i, count, w, h,
                             frame_id, total, uncomp_len, off, ref_id, peer_id,
                             mc_dx, mc_dy, token)
        sock.sendto(hdr + part, addr)
        off += len(part)


def responder(sock, sessions):
    """Single consumer of _resp_queue: turns each GPU result into a response
    (diff-against-client-held-output when possible, keyframe otherwise),
    sends it, and owns the periodic stats printing. Keeping compression out
    of gpu_worker keeps the GPU thread hot for the next frame."""
    zc = zstd.ZstdCompressor(level=1)
    STATS_WINDOW = 30
    while True:
        meta, out = _resp_queue.get()
        sess = sessions.get(meta["addr"])
        if sess is None or sess["token"] != meta["token"]:
            continue  # session died/was replaced while this frame was in the GPU queue

        out_h, out_w = out.shape[2], out.shape[3]
        out_flat = out.reshape(-1)
        out_bytes = out_flat.tobytes()
        uncomp_len = len(out_bytes)

        flags = 0
        ref_id = 0
        mc_dx = mc_dy = 0
        payload = out_bytes
        if meta["compression"]:
            held = meta["held_output_id"]
            ref = sess["outputs"].get(held) if (held and not meta["need_key"]) else None
            if ref is not None and ref[1] == out_w and ref[2] == out_h:
                # Diff against exactly what the client declared holding —
                # never just "the previous frame", so a lost response can
                # only make the next diff a bit bigger, never corrupt it.
                #
                # Motion compensation first: camera scroll makes plain XOR
                # diffs dense exactly when frames are most frequent. Estimate
                # the global shift on the INPUT pair (cheap at 256x224),
                # apply it x3 to the output reference, zero-filled — the
                # diff collapses back to the revealed strip + sprites.
                diff = np.bitwise_xor(out_flat, ref[0])
                plain_density = np.count_nonzero(diff[::64])
                # Shift candidate: prefer the CLIENT's own winning shift
                # (scaled from its reference frame to ours — with steady
                # velocity, shift is proportional to how many frames apart
                # the reference is) — it rides in every MC request header
                # and makes the FFT estimate unnecessary on steady scroll,
                # which was ~1.5ms of every encode. FFT only runs when the
                # client had no shift to offer (scene cuts, raw fallbacks).
                idx = idy = 0
                fid = meta["frame_id"]
                # Quiet frames first: if the plain diff is already near-empty
                # (< ~2% of samples nonzero), no shift can meaningfully
                # improve it — skip ALL shift work, including the FFT
                # fallback, which was otherwise running every frame of
                # dialogue/menu scenes for nothing (measured: 3.6ms encode
                # on quiet windows vs 1.1ms on scroll windows, inverted from
                # what those frames deserve).
                if plain_density * 50 >= len(diff[::64]):
                    if (meta["req_dx"] or meta["req_dy"]) and meta["req_ref"] and fid > meta["req_ref"]:
                        scale = (fid - held) / (fid - meta["req_ref"])
                        idx = int(round(meta["req_dx"] * scale))
                        idy = int(round(meta["req_dy"] * scale))
                    if idx == 0 and idy == 0:
                        in_cur = sess["inputs"].get(fid)
                        in_ref = sess["inputs"].get(held)
                        if (in_cur is not None and in_ref is not None
                                and in_cur[1] == in_ref[1] and in_cur[2] == in_ref[2]):
                            idx, idy = estimate_shift(
                                in_ref[0].reshape(in_ref[2], in_ref[1]),
                                in_cur[0].reshape(in_cur[2], in_cur[1]))
                if (idx or idy) and abs(idx) <= MC_MAX_INPUT_SHIFT and abs(idy) <= MC_MAX_INPUT_SHIFT:
                    # Final arbiter is sampled XOR density — the actual
                    # thing compressed size tracks. Covers both a wrong
                    # scaled client shift and parallax layers that pass an
                    # estimate but don't actually sparsify the diff.
                    shifted = shifted_plane(ref[0], out_h, out_w, idy * 3, idx * 3)
                    mc_diff = np.bitwise_xor(out_flat, shifted)
                    if (np.count_nonzero(mc_diff[::64])
                            < np.count_nonzero(diff[::64])):
                        diff = mc_diff
                        mc_dx, mc_dy = idx * 3, idy * 3
                        flags |= FLAG_MC
                payload = zc.compress(diff.tobytes())
                flags |= FLAG_COMP | FLAG_DIFF
                ref_id = held
            else:
                payload = zc.compress(out_bytes)
                flags = FLAG_COMP
            if len(payload) >= uncomp_len:
                # zstd expanded it (adversarial content) — raw is strictly better
                payload, flags, ref_id, mc_dx, mc_dy = out_bytes, 0, 0, 0, 0
        t_comp = time.perf_counter()

        send_chunked(sock, meta["addr"], T_FRAME_RESP, flags, out_w, out_h,
                     meta["frame_id"], payload, uncomp_len, ref_id,
                     sess["latest_input_id"], sess["token"], mc_dx, mc_dy)
        t_send = time.perf_counter()

        # Response ring: store regardless of how this one was encoded — it's
        # what future client-held references will point at.
        ring_put(sess["outputs"], meta["frame_id"], (out_flat.copy(), out_w, out_h))

        st = sess["stats"]
        st["infer"].append((meta["t_infer"] - meta["t_complete"]) * 1000)
        st["total"].append((t_send - meta["t_complete"]) * 1000)
        st["resp_kb"].append(len(payload) / 1024.0)
        st["decode"].append((meta["t_enqueue"] - meta["t_complete"]) * 1000)
        st["qwait"].append((meta["t_dequeue"] - meta["t_enqueue"]) * 1000)
        st["gpu"].append((meta["t_infer"] - meta["t_dequeue"]) * 1000)
        st["encode"].append((t_send - meta["t_infer"]) * 1000)
        if meta.get("ka_collision"):
            st["ka_collisions"] += 1
        if flags & FLAG_DIFF:
            st["diff_frames"] += 1
        else:
            st["key_frames"] += 1
        if flags & FLAG_MC:
            st["mc_frames"] = st.get("mc_frames", 0) + 1

        if len(st["infer"]) >= STATS_WINDOW:
            n = len(st["infer"])
            print(f"[t] {sess['label']}: last {n} frames — "
                  f"inference avg/min/max: {sum(st['infer'])/n:.2f}/"
                  f"{min(st['infer']):.2f}/{max(st['infer']):.2f}ms, "
                  f"server-side total avg/min/max: {sum(st['total'])/n:.2f}/"
                  f"{min(st['total']):.2f}/{max(st['total']):.2f}ms (excludes network transit)")
            print(f"[t] {sess['label']}: split — decode {sum(st['decode'])/n:.2f}ms, "
                  f"qwait {sum(st['qwait'])/n:.2f}ms (max {max(st['qwait']):.2f}), "
                  f"gpu {sum(st['gpu'])/n:.2f}ms (max {max(st['gpu']):.2f}), "
                  f"encode+send {sum(st['encode'])/n:.2f}ms, "
                  f"keepalive collisions {st['ka_collisions']}/{n}")
            print(f"[t] {sess['label']}: response avg {sum(st['resp_kb'])/n:.1f}KB "
                  f"({st['diff_frames']} diff / {st['key_frames']} key, "
                  f"{st.get('mc_frames', 0)} motion-compensated), "
                  f"requests {st['req_diff_frames']} diff / {st['req_raw_frames']} raw "
                  f"({st.get('req_mc_frames', 0)} mc)")
            st["mc_frames"] = 0
            st["req_mc_frames"] = 0
            st["decode"].clear()
            st["qwait"].clear()
            st["gpu"].clear()
            st["encode"].clear()
            st["ka_collisions"] = 0
            st["infer"].clear()
            st["total"].clear()
            st["resp_kb"].clear()
            st["diff_frames"] = st["key_frames"] = 0
            st["req_diff_frames"] = st["req_raw_frames"] = 0


# ---- optional frame dumping (training-data capture) ------------------------
# Set FRAME_DUMP_DIR to save each NEW unique input frame as a grayscale PNG
# into that directory — raw material for distillation training (see
# native/tools/distill/). Deduped by content hash (in-memory + on-disk, so
# restarts don't rewrite), rate-limited to FRAME_DUMP_MAX_PER_S. Zero cost
# when the env var is unset: the hot path pays one falsy check.

FRAME_DUMP_DIR = os.environ.get("FRAME_DUMP_DIR")
FRAME_DUMP_MAX_PER_S = 20
_dump_seen = set()
_dump_stamps = []
if FRAME_DUMP_DIR:
    os.makedirs(FRAME_DUMP_DIR, exist_ok=True)


def _gray_png_bytes(plane):
    """Minimal 8-bit grayscale PNG encoder — stdlib only, so the server
    gains no Pillow dependency for an opt-in debug feature."""
    h, w = plane.shape
    raw = b"".join(b"\x00" + plane[y].tobytes() for y in range(h))

    def chunk(tag, body):
        c = tag + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6))
            + chunk(b"IEND", b""))


def maybe_dump_frame(luma, w, h):
    """Called from the recv thread with the decoded input plane. The ring in
    the session already dedups by frame_id; this dedups by CONTENT hash so
    static scenes don't flood the dir, and rate-limits so scrolling scenes
    don't either (PNG-encoding every frame at 60fps would also start eating
    into the recv thread's budget)."""
    now = time.monotonic()
    while _dump_stamps and now - _dump_stamps[0] > 1.0:
        _dump_stamps.pop(0)
    if len(_dump_stamps) >= FRAME_DUMP_MAX_PER_S:
        return
    digest = hashlib.sha1(luma).hexdigest()[:16]
    if digest in _dump_seen:
        return
    _dump_seen.add(digest)
    path = os.path.join(FRAME_DUMP_DIR, f"frame_{w}x{h}_{digest}.png")
    if os.path.exists(path):
        return
    _dump_stamps.append(now)
    try:
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(_gray_png_bytes(luma.reshape(h, w)))
        os.replace(tmp, path)
    except OSError as e:
        print(f"[!] frame dump failed ({path}): {e}")


def handle_request_complete(sock, sess, frame_id, flags, w, h, buf, uncomp_len,
                            ref_id, peer_id, zd, t_first, mc_dx=0, mc_dy=0):
    """Runs in the recv thread once every chunk of a request has landed:
    decode (decompress / apply diff, shifted when the client sent a
    motion-compensated diff), stash in the input ring, enqueue for the GPU
    worker."""
    t_complete = time.perf_counter()
    data = bytes(buf)

    if flags & FLAG_COMP:
        try:
            data = zd.decompress(data, max_output_size=uncomp_len)
        except zstd.ZstdError:
            return
        if len(data) != uncomp_len:
            return

    if flags & FLAG_DIFF:
        ref = sess["inputs"].get(ref_id)
        if ref is None or ref[1] != w or ref[2] != h:
            # Can't decode — tell the client immediately so it falls back to
            # raw instead of waiting out a timeout. peer_id=0 resets its ACK.
            hdr = FRAME_HDR.pack(NET_MAGIC, T_FRAME_RESP, FLAG_NACK, 0, 1,
                                 0, 0, frame_id, 0, 0, 0, 0, 0, 0, 0, sess["token"])
            sock.sendto(hdr, sess["addr"])
            print(f"[!] {sess['label']}: request diff vs missing input {ref_id} — NACKed")
            return
        ref_plane = ref[0]
        if (flags & FLAG_MC) and (mc_dx or mc_dy):
            # Client-predicted scroll shift (from controller direction and/or
            # our own last reported shift) — mirror of the response path:
            # rebuild the exact shifted reference the client diffed against.
            ref_plane = shifted_plane(ref[0], h, w, mc_dy, mc_dx)
            sess["stats"]["req_mc_frames"] = sess["stats"].get("req_mc_frames", 0) + 1
        luma = np.bitwise_xor(np.frombuffer(data, dtype=np.uint8), ref_plane)
        sess["stats"]["req_diff_frames"] += 1
    else:
        luma = np.frombuffer(data, dtype=np.uint8)
        sess["stats"]["req_raw_frames"] += 1

    ring_put(sess["inputs"], frame_id, (luma.copy(), w, h))
    if frame_id > sess["latest_input_id"]:
        sess["latest_input_id"] = frame_id
    if FRAME_DUMP_DIR:
        maybe_dump_frame(luma, w, h)

    meta = {
        "addr": sess["addr"],
        "token": sess["token"],
        "frame_id": frame_id,
        "compression": sess["compression"],
        "held_output_id": peer_id,
        "need_key": bool(flags & FLAG_NEED_KEY),
        # The client's own winning scroll shift (and what it was relative
        # to) — lets the responder skip the FFT motion estimate entirely on
        # steady scroll; see the responder's candidate logic.
        "req_dx": mc_dx if (flags & FLAG_MC) else 0,
        "req_dy": mc_dy if (flags & FLAG_MC) else 0,
        "req_ref": ref_id if (flags & FLAG_DIFF) else 0,
        "t_first": t_first,
        "t_complete": t_complete,
        "t_enqueue": time.perf_counter(),
    }
    _gpu_queue.put((luma, h, w, meta))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", required=True, help="path to espcn_x3.bin (see native/tools/package_weights.py)")
    ap.add_argument("--pairing-code", required=True, help="shared secret — enter the same value on the Switch")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9876)
    args = ap.parse_args()

    backend = make_inference_backend(args.weights)
    print(f"[i] inference backend: {backend.name}, weights from {args.weights}")
    if FRAME_DUMP_DIR:
        print(f"[i] FRAME_DUMP_DIR set — dumping unique input frames to "
              f"{FRAME_DUMP_DIR} (max {FRAME_DUMP_MAX_PER_S}/s)")

    threading.Thread(target=gpu_worker, args=(backend,), daemon=True).start()
    if backend.wants_keepalive:
        print("[i] GPU worker started (idles into keep-alive pings when there's no "
              "real work — mitigates the idle-GPU wake-up cost, see pytorch/pytorch#124056)")
    else:
        print("[i] GPU worker started")

    psk_key = derive_psk_key(args.pairing_code)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    sock.bind((args.host, args.port))
    sock.settimeout(1.0)  # lets the loop run GC even when fully idle
    print(f"[i] UDP listening on {args.host}:{args.port} — enter this machine's LAN IP "
          f"and the pairing code on the Switch's Network AI Upscale setup screen")

    sessions = {}   # addr -> session dict
    pending = {}    # addr -> (challenge, monotonic time)
    zd = zstd.ZstdDecompressor()
    last_gc = time.monotonic()

    threading.Thread(target=responder, args=(sock, sessions), daemon=True).start()

    while True:
        try:
            data, addr = sock.recvfrom(65535)
        except socket.timeout:
            data, addr = None, None
        now = time.monotonic()

        if now - last_gc > 5.0:
            last_gc = now
            for a in [a for a, s in sessions.items() if now - s["last_seen"] > SESSION_IDLE_S]:
                print(f"[-] {sessions[a]['label']}: idle {SESSION_IDLE_S:.0f}s, session dropped")
                del sessions[a]
            for a in [a for a, (_, t) in pending.items() if now - t > PENDING_CHALLENGE_TTL_S]:
                del pending[a]
            for s in sessions.values():
                stale = [fid for fid, p in s["partials"].items()
                         if now - p["t_mono"] > PARTIAL_TTL_S]
                for fid in stale:
                    del s["partials"][fid]

        if data is None or len(data) < 5 or data[:4] != NET_MAGIC:
            continue
        mtype = data[4]

        if mtype == T_HELLO:
            challenge = os.urandom(CHALLENGE_LEN)
            pending[addr] = (challenge, now)
            sock.sendto(NET_MAGIC + bytes([T_CHALLENGE]) + challenge, addr)

        elif mtype == T_AUTH and len(data) >= 5 + HMAC_LEN + 1:
            entry = pending.pop(addr, None)
            if entry is None:
                sock.sendto(NET_MAGIC + bytes([T_REJECT]), addr)
                continue
            challenge, _ = entry
            expected = hmac.new(psk_key, challenge, hashlib.sha256).digest()
            if not hmac.compare_digest(data[5:5 + HMAC_LEN], expected):
                print(f"[!] {addr[0]}:{addr[1]}: wrong pairing code, rejected")
                sock.sendto(NET_MAGIC + bytes([T_REJECT]), addr)
                continue
            compression = data[5 + HMAC_LEN] != 0
            token = int.from_bytes(os.urandom(8), "little")
            sessions[addr] = make_session(addr, token, compression)
            sock.sendto(NET_MAGIC + bytes([T_ACCEPT]) + struct.pack("<Q", token)
                        + bytes([1 if compression else 0]), addr)
            print(f"[+] {addr[0]}:{addr[1]}: paired, compression "
                  f"{'on' if compression else 'off'}")

        elif mtype == T_PING and len(data) >= 13:
            sess = sessions.get(addr)
            (token,) = struct.unpack_from("<Q", data, 5)
            if sess is not None and sess["token"] == token:
                sess["last_seen"] = now

        elif mtype == T_FRAME_REQ and len(data) >= FRAME_HDR.size:
            try:
                (_, _, flags, chunk_idx, chunk_count, w, h, frame_id,
                 total_len, uncomp_len, offset, ref_id, peer_id, mc_dx, mc_dy,
                 token) = FRAME_HDR.unpack_from(data)
            except struct.error:
                continue
            if (mc_dx < -MC_MAX_INPUT_SHIFT or mc_dx > MC_MAX_INPUT_SHIFT or
                    mc_dy < -MC_MAX_INPUT_SHIFT or mc_dy > MC_MAX_INPUT_SHIFT):
                continue
            sess = sessions.get(addr)
            if sess is None or sess["token"] != token:
                # Unknown session (server restarted, session GC'd) — tell the
                # client so it re-handshakes now instead of timing out 8 frames.
                sock.sendto(NET_MAGIC + bytes([T_REJECT]), addr)
                continue
            sess["last_seen"] = now

            payload = data[FRAME_HDR.size:]
            if (w == 0 or h == 0 or w > MAX_W or h > MAX_H
                    or uncomp_len != w * h
                    or total_len == 0 or total_len > 2 * MAX_W * MAX_H
                    or chunk_count == 0 or chunk_count > 64
                    or chunk_idx >= chunk_count
                    or offset > total_len
                    or len(payload) > total_len - offset):
                continue

            if chunk_count == 1 and offset == 0 and len(payload) == total_len:
                # Common case: whole request in one datagram, no partial needed.
                handle_request_complete(sock, sess, frame_id, flags, w, h,
                                        payload, uncomp_len, ref_id, peer_id,
                                        zd, time.perf_counter(), mc_dx, mc_dy)
                continue

            p = sess["partials"].get(frame_id)
            if p is None:
                if len(sess["partials"]) >= 2:
                    del sess["partials"][min(sess["partials"].keys())]
                p = {"buf": bytearray(total_len), "got": set(),
                     "count": chunk_count, "flags": flags, "w": w, "h": h,
                     "uncomp_len": uncomp_len, "ref_id": ref_id,
                     "peer_id": peer_id, "total": total_len,
                     "mc_dx": mc_dx, "mc_dy": mc_dy,
                     "t_first": time.perf_counter(), "t_mono": now}
                sess["partials"][frame_id] = p
            if (p["count"] != chunk_count or p["total"] != total_len
                    or p["w"] != w or p["h"] != h):
                del sess["partials"][frame_id]
                continue
            if chunk_idx not in p["got"]:
                p["buf"][offset:offset + len(payload)] = payload
                p["got"].add(chunk_idx)
            if len(p["got"]) == p["count"]:
                del sess["partials"][frame_id]
                handle_request_complete(sock, sess, frame_id, p["flags"], w, h,
                                        p["buf"], p["uncomp_len"], p["ref_id"],
                                        p["peer_id"], zd, p["t_first"],
                                        p["mc_dx"], p["mc_dy"])


if __name__ == "__main__":
    main()
