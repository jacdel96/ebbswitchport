#!/usr/bin/env python3
"""Link micro-benchmark server — the Mac half of native/netbench (Switch NRO).

Measures the raw Switch<->Mac link in isolation: no crypto, no compression,
no inference — just UDP/TCP round trips and blasts. Exists to answer, with
direct hardware numbers instead of theory:
  1. The per-round-trip latency floor of the link (tiny UDP payloads).
  2. Whether the Switch's stack can send/receive >MTU UDP datagrams
     (IP fragmentation) — decides whether protocol v2 must chunk requests.
  3. The real ingress/egress bandwidth ceiling and loss behavior at line
     rate (tests the USB2-adapter-bottleneck hypothesis directly).
  4. TCP vs UDP round trips at the real protocol's payload shapes.

SECURITY NOTE: no auth — this echoes/blasts for anyone who sends to it.
Run it only on the direct cable / a trusted network, and only while testing.

UDP protocol (all little-endian, first 4 bytes = command tag):
  PING [seq u32][fill...]            -> echoed back verbatim
  BLS1 [size u32][count u32][pace_us u32]
                                     -> server sends `count` datagrams of
                                        `size` total bytes: BDAT [seq u32][fill],
                                        then BEND [count u32][elapsed_us u32] x3
  EGRB [count u32]                   -> arm egress counting for this client
  EDAT [seq u32][fill...]            -> egress data (counted, not replied)
  EGRQ                               -> EGRS [received u32][elapsed_us u32]
  RSP1 [seq u32]                     -> 180000B reply as 11 x 16KB chunks:
                                        RDAT [seq u32][idx u16][cnt u16][fill]
  RSP2 [seq u32]                     -> same 180000B as 3 x 60KB chunks

TCP protocol (same port): [len u32][mode u8][payload len bytes] per request;
  mode 0 -> reply [len u32][same payload]  (echo)
  mode 1 -> reply [180000 u32][180000B fill]  (response-shaped reply)

Usage:  python3 net_bench_server.py [--port 9877]
"""
import argparse
import errno
import os
import socket
import struct
import threading
import time

TOTAL_RESP = 180000  # ~ the real protocol's compressed response size
RESP_FILL = os.urandom(TOTAL_RESP)


def send_dgram(sock, data, addr):
    """sendto with ENOBUFS retry — macOS routinely returns ENOBUFS when a UDP
    blast outruns the NIC queue; without the retry, 'sent N datagrams' would
    silently mean 'attempted N', poisoning the loss numbers."""
    for _ in range(5000):
        try:
            sock.sendto(data, addr)
            return True
        except OSError as e:
            if e.errno != errno.ENOBUFS:
                raise
            time.sleep(0.0001)
    return False


def udp_serve(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", port))
    print(f"[i] UDP bench listening on :{port}")

    egress = {}  # addr -> counters

    while True:
        data, addr = sock.recvfrom(65535)
        if len(data) < 4:
            continue
        tag = data[:4]

        if tag == b"PING":
            send_dgram(sock, data, addr)

        elif tag == b"BLS1" and len(data) >= 16:
            size, count, pace_us = struct.unpack_from("<III", data, 4)
            size = max(12, min(size, 65507))
            count = min(count, 20000)
            print(f"[blast] {addr[0]}:{addr[1]} size={size} count={count} pace={pace_us}us")
            fill = b"\xab" * (size - 8)
            sent = 0
            t0 = time.perf_counter()
            for seq in range(count):
                if send_dgram(sock, b"BDAT" + struct.pack("<I", seq) + fill, addr):
                    sent += 1
                if pace_us:
                    time.sleep(pace_us / 1e6)
            elapsed = time.perf_counter() - t0
            end = b"BEND" + struct.pack("<II", sent, int(elapsed * 1e6))
            for _ in range(3):
                send_dgram(sock, end, addr)
                time.sleep(0.02)
            mbps = sent * size * 8 / elapsed / 1e6 if elapsed > 0 else 0
            print(f"[blast] sent {sent}/{count} in {elapsed*1000:.1f}ms "
                  f"({mbps:.0f} Mbps offered)")

        elif tag == b"EGRB" and len(data) >= 8:
            (count,) = struct.unpack_from("<I", data, 4)
            egress[addr] = {"count": count, "received": 0, "bytes": 0,
                            "first": None, "last": None}
            print(f"[egress] {addr[0]}:{addr[1]} armed, expecting {count}")

        elif tag == b"EDAT":
            st = egress.get(addr)
            if st is not None:
                now = time.perf_counter()
                st["received"] += 1
                st["bytes"] += len(data)
                if st["first"] is None:
                    st["first"] = now
                st["last"] = now

        elif tag == b"EGRQ":
            st = egress.get(addr)
            if st is not None:
                elapsed_us = 0
                if st["first"] is not None and st["last"] > st["first"]:
                    elapsed_us = int((st["last"] - st["first"]) * 1e6)
                send_dgram(sock, b"EGRS" + struct.pack("<II", st["received"], elapsed_us), addr)
                mbps = st["bytes"] * 8 / elapsed_us if elapsed_us else 0
                print(f"[egress] {addr[0]}:{addr[1]}: {st['received']}/{st['count']} received"
                      f" ({mbps:.0f} Mbps over {elapsed_us/1000:.1f}ms)")

        elif tag in (b"RSP1", b"RSP2") and len(data) >= 8:
            seq = data[4:8]
            chunk = 16384 if tag == b"RSP1" else 61440
            cnt = (TOTAL_RESP + chunk - 1) // chunk
            off = 0
            for i in range(cnt):
                part = RESP_FILL[off:off + chunk]
                off += len(part)
                send_dgram(sock, b"RDAT" + seq + struct.pack("<HH", i, cnt) + part, addr)


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def tcp_conn_thread(conn, addr):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    rounds = 0
    try:
        while True:
            hdr = recv_exact(conn, 5)
            if hdr is None:
                break
            length, mode = struct.unpack("<IB", hdr)
            if length > 2 * 1024 * 1024:
                break
            payload = recv_exact(conn, length)
            if payload is None:
                break
            reply = payload if mode == 0 else RESP_FILL
            conn.sendall(struct.pack("<I", len(reply)) + reply)
            rounds += 1
    finally:
        conn.close()
        print(f"[tcp] {addr[0]}:{addr[1]} closed after {rounds} rounds")


def tcp_serve(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(2)
    print(f"[i] TCP bench listening on :{port}")
    while True:
        conn, addr = srv.accept()
        print(f"[tcp] connection from {addr[0]}:{addr[1]}")
        threading.Thread(target=tcp_conn_thread, args=(conn, addr), daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9877)
    args = ap.parse_args()

    threading.Thread(target=tcp_serve, args=(args.port,), daemon=True).start()
    udp_serve(args.port)


if __name__ == "__main__":
    main()
