#!/usr/bin/env python3
"""Writes a synthetic walk-in-place session in pipe_probe log format, for fake_bridge.py."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pipe_probe

Msg = pipe_probe.load_msg_class()
treadmill = len(sys.argv) < 3 or sys.argv[2] != "inject"
out = []

def emit(t, fill):
    m = Msg()
    fill(m)
    out.append(f"{t:.4f} rx {m.SerializeToString().hex()}")

def setting(on):
    def f(m):
        m.wip_setting.open_wip = on
        m.wip_setting.output_as_treadmill = on and treadmill
        m.wip_setting.use_left_hand = True

    return f

def info(x, status):
    def f(m):
        m.wip_info.x = x
        m.wip_info.y = 0.5
        m.wip_info.wip_status = status
        m.wip_info.position.tracker_id = 3

    return f

if len(sys.argv) > 3 and sys.argv[3] == "waist":
    # Waist tracker (id 3, role 0) turned 90 degrees to the right: forward axis = world +X.
    def added(m):
        m.tracker_added.tracker_id = 3
        m.tracker_added.tracker_role = 0
        m.tracker_added.tracker_name = "rebocap_0"

    def status(m):
        m.tracker_status.tracker_id = 3
        m.tracker_status.status = 1
        m.tracker_status.battery_level = 0.9

    def pos(m):
        m.position.tracker_id = 3
        m.position.q.extend([0.70710678, 0, -0.70710678, 0])
        m.position.pos.extend([0, 1.0, 0])

    emit(0.2, added)
    emit(0.3, status)
    for i in range(110):
        emit(0.4 + i * 0.05, pos)

emit(0.5, setting(True))
emit(0.6, setting(True)) # repeated setting must not re-create the device
emit(1.9, info(0.0, 1)) # START_WALKING

# WALKING, speed ramp 0.0 .. 0.9
for i in range(10):
    emit(2.0 + i * 0.05, info(0.1 * i, 3))

emit(2.6, info(1.7, 3)) # out of range, must clamp to 1.0
emit(3.9, info(0.5, 3)) # after >1 s silence the driver must have zeroed the stick
emit(4.1, info(0.5, 2)) # STOPPED forces 0
emit(4.3, info(0.4, 3))
emit(4.5, setting(False)) # disabling must zero the stick and disconnect
emit(5.5, setting(False))

out.sort(key=lambda l: float(l.split()[0]))
open(sys.argv[1], "w").write("\n".join(out) + "\n")
