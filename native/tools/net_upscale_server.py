#!/usr/bin/env python3
"""Laptop-side ESPCN inference server for ebbswitchport's "Network AI Upscale".

Offloads the expensive 3-layer super-resolution network to a real GPU/CPU
instead of the Switch's Tegra X1 (see the research memo: the Switch's own
compute-shader implementation of this same network measured ~312ms/frame —
this laptop measured ~1.3ms/frame on a discrete GPU, ~23ms on CPU alone).

Protocol (TCP, one connection per game session):
  1. Server accepts, sends a 16-byte random challenge.
  2. Client proves it knows the shared pairing code: HMAC-SHA256(psk_key,
     challenge), 32 bytes.
  3. Server verifies (constant-time compare); wrong code closes the socket.
  4. Both sides derive a fresh per-session key via HKDF(psk_key, salt=challenge)
     — this, not the pairing code itself, encrypts every frame, so a captured
     handshake can't be replayed against a later session.
  5. Every message after that is ChaCha20-Poly1305 sealed: [4-byte length
     prefix][12-byte nonce][ciphertext+16-byte tag]. Nonces increment per
     direction and are never reused with the same key.
  6. Frame request (client->server): u16 width, u16 height, raw uint8 luma.
     Frame response (server->client): u16 width, u16 height, raw uint8 luma
     (upscaled by the fixed x3 factor) — chroma is never sent; the Switch
     already has the original RGB and reconstructs color locally, same as
     the local GPU path's espcn3_comp.glsl does today.

Usage:
    python net_upscale_server.py --weights /path/to/espcn_x3.bin \\
        --pairing-code "correct horse battery staple"
"""
import argparse
import hashlib
import hmac
import os
import socket
import struct
import sys
import threading
import time
import zlib

import numpy as np
import torch
import torch.nn as nn
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes

MAGIC = b"ESP1"
CHALLENGE_LEN = 16
HMAC_LEN = 32
NONCE_LEN = 12
TAG_LEN = 16


class ESPCN(nn.Module):
    """Matches native/source/shaders/espcn{1,2,3}_comp.glsl exactly — same
    layer shapes, same tanh placement, same lack of activation on layer 3."""

    def __init__(self):
        super().__init__()
        self.conv1 = nn.Conv2d(1, 64, 5, padding=2)
        self.conv2 = nn.Conv2d(64, 32, 3, padding=1)
        self.conv3 = nn.Conv2d(32, 9, 3, padding=1)
        self.shuffle = nn.PixelShuffle(3)
        self.tanh = nn.Tanh()

    def forward(self, x):
        x = self.tanh(self.conv1(x))
        x = self.tanh(self.conv2(x))
        x = self.conv3(x)
        return self.shuffle(x)


def load_weights(model, bin_path):
    """Loads native/tools/package_weights.py's binary format directly —
    struct/numpy only, no torch.load/pickle involved for this file."""
    with open(bin_path, "rb") as f:
        magic = f.read(4)
        if magic != MAGIC:
            raise ValueError(f"bad magic {magic!r}, expected {MAGIC!r}")
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


def derive_psk_key(pairing_code: str) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(), length=32,
        salt=b"ebbswitchport-espcn-psk", info=b"psk",
    ).derive(pairing_code.encode("utf-8"))


def derive_session_key(psk_key: bytes, challenge: bytes) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(), length=32,
        salt=challenge, info=b"ebbswitchport-espcn-session",
    ).derive(psk_key)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed connection")
        buf += chunk
    return bytes(buf)


def nonce_bytes(counter: int) -> bytes:
    return counter.to_bytes(NONCE_LEN, "little")


class SecureChannel:
    """One AEAD key, independent monotonic nonce counters per direction —
    never reuses a (key, nonce) pair, which is the one hard rule ChaCha20
    correctness depends on."""

    def __init__(self, sock, session_key: bytes):
        self.sock = sock
        self.aead = ChaCha20Poly1305(session_key)
        self.send_counter = 0
        self.recv_counter = 0

    def send(self, plaintext: bytes):
        nonce = nonce_bytes(self.send_counter)
        self.send_counter += 1
        ct = self.aead.encrypt(nonce, plaintext, None)
        frame = nonce + ct
        self.sock.sendall(struct.pack("<I", len(frame)) + frame)

    def recv(self) -> bytes:
        (length,) = struct.unpack("<I", recv_exact(self.sock, 4))
        frame = recv_exact(self.sock, length)
        nonce, ct = frame[:NONCE_LEN], frame[NONCE_LEN:]
        expected_nonce = nonce_bytes(self.recv_counter)
        if nonce != expected_nonce:
            raise ValueError("nonce out of sequence — dropped/reordered/replayed message")
        self.recv_counter += 1
        return self.aead.decrypt(nonce, ct, None)


def handshake(sock, psk_key: bytes) -> SecureChannel:
    challenge = os.urandom(CHALLENGE_LEN)
    sock.sendall(challenge)
    response = recv_exact(sock, HMAC_LEN)
    expected = hmac.new(psk_key, challenge, hashlib.sha256).digest()
    if not hmac.compare_digest(response, expected):
        raise PermissionError("wrong pairing code")
    session_key = derive_session_key(psk_key, challenge)
    return SecureChannel(sock, session_key)


def handle_client(sock, addr, psk_key, model, device):
    print(f"[+] connection from {addr}, awaiting handshake...")
    try:
        channel = handshake(sock, psk_key)
    except PermissionError:
        print(f"[!] {addr}: wrong pairing code, closing")
        sock.close()
        return
    except (ConnectionError, ValueError) as e:
        print(f"[!] {addr}: handshake failed ({e}), closing")
        sock.close()
        return
    print(f"[+] {addr}: paired, session key established")

    frame_count = 0
    STATS_WINDOW = 30  # print a summary every N frames rather than spamming per-frame
    infer_times = []   # pure model forward time (recv already decrypted -> tensor ready)
    total_times = []   # recv-decrypted to send-encrypted-and-flushed (server-side only —
                        # does NOT include network transit either direction; that's
                        # whatever gap the Switch itself measures around the whole round trip)
    comp_ratios = []   # compressed / original size of each response, for visibility
    # Granular per-stage breakdown, since isolated single-threaded benchmarks
    # (bench_realistic.py, bench_zlib.py) don't reproduce whatever the live
    # multi-threaded server process actually does — this measures the real
    # thing instead of guessing from a synthetic replica.
    stage_times = {"decompress": [], "convert+xfer": [], "forward": [], "postprocess": [], "compress": [], "send": []}
    try:
        while True:
            msg = channel.recv()
            t_recv_done = time.perf_counter()

            w, h, ulen = struct.unpack("<HHI", msg[:8])
            luma_bytes = zlib.decompress(msg[8:])
            if len(luma_bytes) != ulen or len(luma_bytes) != w * h:
                raise ValueError(f"expected {w*h} luma bytes, got {len(luma_bytes)} (ulen={ulen})")
            luma = np.frombuffer(luma_bytes, dtype=np.uint8)
            t_decompress_done = time.perf_counter()

            x = torch.from_numpy(luma.reshape(1, 1, h, w).astype(np.float32) / 255.0)
            x = x.to(device)
            t_convert_done = time.perf_counter()

            with torch.no_grad():
                y = model(x)
                if device.type == "mps":
                    torch.mps.synchronize()
            t_infer_done = time.perf_counter()

            out = (y.clamp(0, 1) * 255.0).round().to(torch.uint8).cpu().numpy()
            out_h, out_w = out.shape[2], out.shape[3]
            out_bytes = out.tobytes()
            t_postprocess_done = time.perf_counter()

            # level 1: fastest setting — we have compute headroom (inference
            # is ~9ms) to spend a few ms shrinking what's actually the
            # dominant cost, the WiFi transfer of this response.
            comp = zlib.compress(out_bytes, level=1)
            t_compress_done = time.perf_counter()

            reply = struct.pack("<HHI", out_w, out_h, len(out_bytes)) + comp
            channel.send(reply)
            t_send_done = time.perf_counter()

            frame_count += 1
            infer_times.append((t_infer_done - t_recv_done) * 1000)
            total_times.append((t_send_done - t_recv_done) * 1000)
            comp_ratios.append(len(comp) / len(out_bytes))
            stage_times["decompress"].append((t_decompress_done - t_recv_done) * 1000)
            stage_times["convert+xfer"].append((t_convert_done - t_decompress_done) * 1000)
            stage_times["forward"].append((t_infer_done - t_convert_done) * 1000)
            stage_times["postprocess"].append((t_postprocess_done - t_infer_done) * 1000)
            stage_times["compress"].append((t_compress_done - t_postprocess_done) * 1000)
            stage_times["send"].append((t_send_done - t_compress_done) * 1000)
            if len(infer_times) >= STATS_WINDOW:
                print(f"[t] {addr}: last {STATS_WINDOW} frames — "
                      f"inference avg/min/max: {sum(infer_times)/len(infer_times):.2f}/"
                      f"{min(infer_times):.2f}/{max(infer_times):.2f}ms, "
                      f"server-side total avg/min/max: {sum(total_times)/len(total_times):.2f}/"
                      f"{min(total_times):.2f}/{max(total_times):.2f}ms (excludes network transit), "
                      f"response compressed to {sum(comp_ratios)/len(comp_ratios)*100:.0f}% of original size avg")
                breakdown = ", ".join(f"{k} {sum(v)/len(v):.2f}ms" for k, v in stage_times.items())
                print(f"[t] {addr}: stage breakdown (avg) — {breakdown}")
                for v in stage_times.values():
                    v.clear()
                infer_times.clear()
                total_times.clear()
                comp_ratios.clear()
    except ConnectionError:
        print(f"[-] {addr}: disconnected after {frame_count} frames")
    except Exception as e:
        print(f"[!] {addr}: error after {frame_count} frames: {e}")
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", required=True, help="path to espcn_x3.bin (see native/tools/package_weights.py)")
    ap.add_argument("--pairing-code", required=True, help="shared secret — enter the same value on the Switch")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9876)
    args = ap.parse_args()

    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    print(f"[i] using device: {device}")

    model = ESPCN().to(device).eval()
    load_weights(model, args.weights)
    print(f"[i] weights loaded from {args.weights}")

    psk_key = derive_psk_key(args.pairing_code)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print(f"[i] listening on {args.host}:{args.port} — enter this machine's LAN IP "
          f"and the pairing code on the Switch's Network AI Upscale setup screen")

    try:
        while True:
            sock, addr = srv.accept()
            # Disable Nagle's algorithm — this is a tight request/response
            # protocol (one small request, wait for one response), and Nagle
            # + the peer's delayed-ACK timer is a well-known source of
            # tens-of-ms added latency per round trip otherwise.
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            t = threading.Thread(target=handle_client, args=(sock, addr, psk_key, model, device), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\n[i] shutting down")
        srv.close()


if __name__ == "__main__":
    main()
