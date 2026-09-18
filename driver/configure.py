#!/usr/bin/env python3
"""Shows or changes the driver's settings in SteamVR's steamvr.vrsettings.

  ./configure.py                              show current values
  ./configure.py wip_direction waist          set a value
  ./configure.py --unset wip_direction        back to the default

Settings (section "driver_rebocap"):
  bridge_host, bridge_port   bridge.exe listening address (default 127.0.0.1:36850)
  universe_fallback          use raw tracking space if the play space cannot be found (default true)
  input_hooks                let walk-in-place take over the hand controller's stick (default true)
  wip_direction              "game" - stick forward, game-dependent (default)
                             "waist" - walk where the waist tracker faces
  wip_game_reference         only for "waist" - what the game treats as forward,
                             "head" (default) or "controller" (e.g. H3VR relative movement)
SteamVR reads the file at startup and rewrites it on exit, so change settings only while it is closed.
"""

import json
import os
import subprocess
import sys

SECTION = "driver_rebocap"

path = os.path.expanduser(os.environ.get("STEAMVR_SETTINGS", "~/.local/share/Steam/config/steamvr.vrsettings"))
data = json.load(open(path)) if os.path.exists(path) else {}
args = sys.argv[1:]

if not args:
    print(path)
    print(json.dumps(data.get(SECTION, {}), indent=2) if data.get(SECTION) else "(all defaults)")
    sys.exit(0)

if subprocess.run(["pgrep", "-x", "vrserver"], capture_output=True).returncode == 0:
    sys.exit("SteamVR is running, cannot change settings while it is running.")

if args[0] == "--unset" and len(args) == 2:
    data.get(SECTION, {}).pop(args[1], None)
elif len(args) == 2:
    try:
        value = json.loads(args[1])
    except ValueError:
        value = args[1]

    data.setdefault(SECTION, {})[args[0]] = value
else:
    sys.exit(__doc__)

json.dump(data, open(path, "w"), indent=3)
print(json.dumps(data.get(SECTION, {}), indent=2))
