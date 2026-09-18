#!/usr/bin/env python3
"""Replays the app->driver frames of a pipe_probe capture to one TCP client at recorded pace."""

import socket
import struct
import sys
import threading
import time

frames = [(float(t), bytes.fromhex(h)) for t, d, h in (l.split() for l in open(sys.argv[1])) if d == "rx"]

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[2])))
srv.listen(1)
conn, _ = srv.accept()
got = {"n": 0}

def reader():
    while True:
        d = conn.recv(65536)
        if not d:
            return

        got["n"] += len(d)

threading.Thread(target=reader, daemon=True).start()
t0, base = time.monotonic(), frames[0][0]

try:
    for t, data in frames:
        delay = (t - base) - (time.monotonic() - t0)
        if delay > 0:
            time.sleep(delay)

        conn.sendall(struct.pack("<I", len(data)) + data)
except OSError:
    pass

print("fake bridge: received", got["n"], "bytes from driver")
