#!/usr/bin/env python3
"""Synthetic hand pose replacement session in pipe_probe log format, for fake_bridge.py."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pipe_probe

Msg = pipe_probe.load_msg_class()
out = []

def emit(t, fill):
    m = Msg()
    fill(m)
    out.append((t, f"{t:.4f} rx {m.SerializeToString().hex()}"))

def setting(on):
    def f(m):
        m.wip_setting.replace_hand_pose = on
        m.wip_setting.use_left_hand = True

    return f

def hand(m):
    m.position.tracker_id = 16
    m.position.q.extend([1, 0, 0, 0])
    m.position.pos.extend([0.1, 1.2, -0.3])

emit(1.0, setting(True))

# 1.5 - 3.0 s: replaced
for i in range(30):
    emit(1.5 + i * 0.05, hand)

# silent 3.0 - 4.0 s (hand returns after 0.5 s), then replaced again
for i in range(30):
    emit(4.0 + i * 0.05, hand)

emit(4.8, setting(False)) # disabled while poses still arrive, so the real pose returns

open(sys.argv[1], "w").write("\n".join(l for _, l in sorted(out)) + "\n")
