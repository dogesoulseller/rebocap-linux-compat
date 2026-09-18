#!/usr/bin/env python3
"""Minimal Breakpad (Linux x86-64) minidump reader: crash address, module, and return addresses
on the crashing thread's stack that fall inside modules matching a name filter."""

import struct
import sys

d = open(sys.argv[1], "rb").read()
flt = sys.argv[2] if len(sys.argv) > 2 else ""
sig, ver, nstreams, dir_rva = struct.unpack_from("<IIII", d, 0)

streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from("<III", d, dir_rva + 12 * i)
    streams[t] = (size, rva)

def mdstring(rva):
    (n,) = struct.unpack_from("<I", d, rva)
    return d[rva + 4:rva + 4 + n].decode("utf-16le")

mods = []
size, rva = streams[4]
(n,) = struct.unpack_from("<I", d, rva)
for i in range(n):
    o = rva + 4 + 108 * i
    base, msize = struct.unpack_from("<QI", d, o)
    (name_rva,) = struct.unpack_from("<I", d, o + 20)
    mods.append((base, msize, mdstring(name_rva)))

def where(a):
    for b, s, nm in mods:
        if b <= a < b + s:
            return nm, a - b

    return None, a

size, rva = streams[6]
tid, _, code, flags, rec, addr = struct.unpack_from("<IIIIQQ", d, rva)
ctx_size, ctx_rva = struct.unpack_from("<II", d, rva + 160)
(rip,) = struct.unpack_from("<Q", d, ctx_rva + 0xF8)
(rsp,) = struct.unpack_from("<Q", d, ctx_rva + 0x98)
print(f"crash thread {tid} signal {code} fault addr {addr:#x}")

nm, off = where(rip)
print(f"RIP {rip:#x} -> {nm} + {off:#x}")

nm, off = where(addr)
print(f"fault address is in: {nm} + {off:#x}" if nm else "fault address is not inside any module")

size, rva = streams[3]
(n,) = struct.unpack_from("<I", d, rva)
for i in range(n):
    o = rva + 4 + 48 * i
    (t,) = struct.unpack_from("<I", d, o)
    if t != tid:
        continue

    start, msize, mrva = struct.unpack_from("<QII", d, o + 24)
    stack = d[mrva:mrva + msize]
    print(f"stack {start:#x}+{msize:#x}, rsp {rsp:#x}")

    shown = 0
    for k in range(max(0, rsp - start) & ~7, len(stack) - 7, 8):
        (v,) = struct.unpack_from("<Q", stack, k)
        nm, off = where(v)
        if nm and (flt in nm) and shown < 40:
            print(f"  [rsp+{start + k - rsp:#06x}] {v:#x} {nm.split('/')[-1]} + {off:#x}")
            shown += 1
