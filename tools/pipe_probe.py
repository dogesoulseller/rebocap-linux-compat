#!/usr/bin/env python3
"""Stand-in for the Rebocap SteamVR driver, for protocol checks without SteamVR.

Connects to bridge.exe over TCP, prints every message the app sends, and reports a
synthetic headset the way driver_rebocap.dll does (see docs/protocol.md):
DeviceSt{id 0, OK}, then UniverseChange, then Position{id 0} every frame.

usage: pipe_probe.py [--host H] [--port P] [--no-hmd] [--height M] [--log FILE]
"""

import argparse
import math
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory, text_format

PROTO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "proto")

def load_msg_class():
    # Message classes are built at run time from the .proto, so no generated code is checked in.
    with tempfile.TemporaryDirectory() as tmp:
        desc = os.path.join(tmp, "ReboTracker.desc")
        try:
            subprocess.run(["protoc", "-I", PROTO_DIR, "--descriptor_set_out=" + desc,
                            os.path.join(PROTO_DIR, "ReboTracker.proto")], check=True)
        except FileNotFoundError:
            sys.exit("protoc not found; install the protobuf compiler")

        fds = descriptor_pb2.FileDescriptorSet()
        with open(desc, "rb") as f:
            fds.ParseFromString(f.read())

    pool = descriptor_pool.DescriptorPool()
    for fd in fds.file:
        pool.Add(fd)

    return message_factory.GetMessageClass(pool.FindMessageTypeByName("rebocap_pb.RebocapMsg"))

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("bridge closed the connection")

        buf += chunk

    return buf

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=36850)
    ap.add_argument("--no-hmd", action="store_true", help="listen only, send nothing")
    ap.add_argument("--height", type=float, default=1.7, help="synthetic headset height in metres")
    ap.add_argument("--log", help="append raw frames as hex lines: <time> <rx|tx> <hex>")
    args = ap.parse_args()

    Msg = load_msg_class()
    sock = socket.create_connection((args.host, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    log = open(args.log, "a") if args.log else None
    t0 = time.monotonic()

    def send(msg):
        data = msg.SerializeToString()
        sock.sendall(struct.pack("<I", len(data)) + data)
        if log:
            log.write(f"{time.monotonic() - t0:.4f} tx {data.hex()}\n")

    if not args.no_hmd:
        m = Msg()
        m.tracker_status.tracker_id = 0
        m.tracker_status.status = 1
        send(m)
        m = Msg()
        m.uch.yaw = 0.0
        m.uch.pos.extend([0.0, 0.0, 0.0])
        send(m)
        print("sent DeviceSt{0, OK} and UniverseChange{yaw 0, pos 0}")

    sock.settimeout(1 / 90)
    counts = {}
    pos_seen = {}
    last_report = time.monotonic()
    pending = b""
    while True:
        if not args.no_hmd:
            # Slow head sway so the app sees a changing pose: yaw +-20 degrees over 8 s.
            yaw = math.radians(20) * math.sin((time.monotonic() - t0) * 2 * math.pi / 8)
            m = Msg()
            m.position.tracker_id = 0
            m.position.q.extend([math.cos(yaw / 2), 0.0, math.sin(yaw / 2), 0.0])
            m.position.pos.extend([0.0, args.height, 0.0])
            send(m)

        try:
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError("bridge closed the connection")

            pending += chunk
        except socket.timeout:
            pass

        while len(pending) >= 4:
            (n,) = struct.unpack_from("<I", pending)
            if len(pending) < 4 + n:
                break

            data, pending = pending[4:4 + n], pending[4 + n:]
            if log:
                log.write(f"{time.monotonic() - t0:.4f} rx {data.hex()}\n")

            m = Msg()
            m.ParseFromString(data)
            kind = m.WhichOneof("message")
            counts[kind] = counts.get(kind, 0) + 1
            if kind == "position":
                # Positions arrive at framerate. Print the first one per tracker, then summarize.
                first = m.position.tracker_id not in pos_seen
                pos_seen[m.position.tracker_id] = m.position
                if not first:
                    continue

            print(f"[{time.monotonic() - t0:8.3f}] {text_format.MessageToString(m, as_one_line=True)}")

        if time.monotonic() - last_report > 5:
            last_report = time.monotonic()
            print(f"--- counts {counts}")
            for tid, p in sorted(pos_seen.items()):
                print(f"    id {tid:2d} q(wxyz)={[round(v, 3) for v in p.q]} pos={[round(v, 3) for v in p.pos]}")

            sys.stdout.flush()

if __name__ == "__main__":
    try:
        main()
    except (ConnectionError, KeyboardInterrupt) as e:
        print(f"stopped: {e!r}")
