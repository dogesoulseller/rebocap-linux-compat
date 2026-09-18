#!/usr/bin/env python3
"""Installs Rebocap for Linux SteamVR from the vendor's Windows installer.

  ./install.py /path/to/rebocap_release_vXX.exe     install or update including Rebocap itself
  ./install.py                                      update bridge, driver, prefix fixes and launchers
                                                    of an existing install, without moving the vendor app the vendor app is not touched
  ./install.py --uninstall                          unregister the driver and remove launchers
                                                    (excluding install dir)
options:
  --home DIR        install directory (default: $XDG_DATA_HOME/rebocap-linux)
  --com COMn        COM port name given to the dongle inside Wine (default COM5)
  --no-register     do not register the driver with SteamVR or create launchers

The install:
  - Unpacks the vendor app
  - Builds the bridge and native SteamVR driver
  - Creates a Wine prefix with the fixes the app needs, registers the driver with SteamVR, and creates a `rebocap` launcher
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

SRC = os.path.dirname(os.path.abspath(__file__))
DONGLE = "/dev/serial/by-id/usb-JXQ_Ltd__Co_RebornBodyCap_RebornRX-if00"
PORTS_CLASS = "{4d36e978-e325-11ce-bfc1-08002be10318}"

def run(cmd, **kw):
    print("  $", " ".join(cmd) if isinstance(cmd, list) else cmd)
    return subprocess.run(cmd, check=True, **kw)

def need(tool, why):
    if not shutil.which(tool):
        sys.exit(f"missing `{tool}` ({why}); install it and run again")

def steamvr_dir():
    env = os.environ.get("STEAMVR_DIR")
    candidates = [env] if env else []
    reg = os.path.expanduser(os.environ.get("VR_PATHREG_OVERRIDE", "~/.config/openvr/openvrpaths.vrpath"))
    if os.path.exists(reg):
        candidates += json.load(open(reg)).get("runtime", [])

    candidates.append(os.path.expanduser("~/.local/share/Steam/steamapps/common/SteamVR"))
    for c in candidates:
        if c and os.path.exists(os.path.join(c, "bin", "vrpathreg.sh")):
            return c

    sys.exit("SteamVR not found. Install it through Steam or set STEAMVR_DIR")

def registered_rebocap_drivers():
    reg = os.path.expanduser(os.environ.get("VR_PATHREG_OVERRIDE", "~/.config/openvr/openvrpaths.vrpath"))
    if not os.path.exists(reg):
        return []

    found = []
    for path in json.load(open(reg)).get("external_drivers") or []:
        manifest = os.path.join(path, "driver.vrdrivermanifest")
        try:
            if json.load(open(manifest)).get("name") == "rebocap":
                found.append(path)
        except (OSError, ValueError):
            if os.path.basename(path.rstrip("/")) == "rebocap":
                found.append(path) # stale entry whose folder is gone

    return found

def static_font(candidates):
    """Returns the first installed family from `candidates` that has a non-variable face.
    Wine renders variable fonts at their default instance, which is often Thin."""
    try:
        out = subprocess.run(["fc-list", ":", "family", "variable"], capture_output=True, text=True).stdout
    except OSError:
        return None

    static = set()
    for line in out.splitlines():
        if "variable=False" in line:
            static.update(f.strip() for f in line.split(":")[0].split(","))

    return next((c for c in candidates if c in static), None)

def reg_multi_sz(strings):
    data = "".join(s + "\0" for s in strings) + "\0"
    return "hex(7):" + ",".join(f"{b:02x}" for b in data.encode("utf-16le"))

def prefix_registry(com):
    esc = lambda s: s.replace("\\", "\\\\")
    dev = r"HKEY_LOCAL_MACHINE\System\CurrentControlSet\Enum\USB\VID_248A&PID_8002\REBORNRX"
    lines = [
        "REGEDIT4", "",
        # Wine maps serial ports to COMn but doesn't register a device in the Ports class, so pyserial's comports() doesn't find anything.
        # This entry is the device that Windows' usbser driver would show.
        f"[{dev}]",
        f'"ClassGUID"="{PORTS_CLASS}"', '"Class"="Ports"',
        '"HardwareID"=' + reg_multi_sz([r"USB\VID_248A&PID_8002&REV_0100", r"USB\VID_248A&PID_8002"]),
        '"CompatibleIDs"='
        + reg_multi_sz([r"USB\Class_02&SubClass_02&Prot_01", r"USB\Class_02&SubClass_02", r"USB\Class_02"]),
        f'"FriendlyName"="USB Serial Device ({com})"', '"DeviceDesc"="USB Serial Device"',
        '"Mfg"="Microsoft"', '"Service"="usbser"', "",
        f"[{dev}\\Device Parameters]", f'"PortName"="{com}"', "",
        r"[HKEY_LOCAL_MACHINE\Software\Wine\Ports]", f'"{com}"="{esc(DONGLE)}"', "",
        # Bug in Wine? (11.15)
        # SetupDiClassGuidsFromName compares the class description, but Windows uses the class name. Equal values work in both cases.
        rf"[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Control\Class\{PORTS_CLASS}]",
        '@="Ports"', '"Class"="Ports"', "",
        # The 3D viewport is another process's window embedded into the Qt window.
        # With window manager decorations it is offset by the height of the title bar.
        r"[HKEY_CURRENT_USER\Software\Wine\X11 Driver]", '"Decorated"="N"', "",
    ]
    latin = static_font(["Noto Sans", "Open Sans", "DejaVu Sans", "Liberation Sans", "Arimo"])
    cjk = static_font(["Noto Sans CJK SC", "Source Han Sans SC", "WenQuanYi Micro Hei"])
    repl = {}
    if latin:
        repl.update({"Segoe UI": latin, "Segoe UI Semibold": latin})

    if cjk:
        repl.update({"Microsoft YaHei": cjk, "Microsoft YaHei UI": cjk, "Yu Gothic UI": cjk})
    elif latin:
        repl.update({"Microsoft YaHei": latin, "Microsoft YaHei UI": latin, "Yu Gothic UI": latin})

    if repl:
        lines.append(r"[HKEY_CURRENT_USER\Software\Wine\Fonts\Replacements]")
        lines += [f'"{k}"="{v}"' for k, v in repl.items()] + [""]
    else:
        print("  note: no static sans-serif font found (e.g. Noto Sans), so the app's text may render badly")

    return "\r\n".join(lines)

LAUNCHER = r"""#!/bin/bash
# Starts the pipe<->TCP bridge and the Rebocap app. Generated by install.py.
#   BRIDGE_BIND=0.0.0.0 rebocap     let a driver on another machine connect (bridge_host on that side)
home={home}
export WINEPREFIX="$home/pfx" WINEDEBUG="${{WINEDEBUG:--all}}"
[ -e {dongle} ] || echo "rebocap: dongle not found at {dongle}. Plug it in first" >&2
wine "$home/bridge.exe" "${{BRIDGE_BIND:-127.0.0.1}}" "${{BRIDGE_PORT:-36850}}" 2>"$home/bridge.log" &
bridge=$!
trap 'kill $bridge 2>/dev/null' EXIT
# Without the bridge, SteamVR does not see the app, and the error is only in bridge.log.
for _ in $(seq 20); do
    grep -q '\[bridge.*listening' "$home/bridge.log" 2>/dev/null && break
    if grep -q '\[bridge.*cannot listen' "$home/bridge.log" 2>/dev/null; then
        msg="Rebocap: port ${{BRIDGE_PORT:-36850}} is already in use (another bridge.exe still running?). SteamVR will not connect until it is free. Once free, restart rebocap."
        echo "$msg" >&2
        command -v notify-send >/dev/null && notify-send -u critical "Rebocap" "$msg"
        break
    fi
    sleep 0.5
done
cd "$home/app" && wine rebocap.exe
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=Rebocap
Comment=Rebocap motion capture (Wine) with SteamVR bridge
Exec={launcher}
Icon={icon}
Categories=Game;Utility;
"""

def user_paths():
    data = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
    return (os.path.expanduser("~/.local/bin/rebocap"), os.path.expanduser("~/.local/bin/rebocap-config"),
            os.path.join(data, "applications", "rebocap.desktop"))

def uninstall():
    vrpathreg = os.path.join(steamvr_dir(), "bin", "vrpathreg.sh")
    for path in registered_rebocap_drivers():
        run([vrpathreg, "removedriver", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    for p in user_paths():
        if os.path.lexists(p):
            os.remove(p)

    print("unregistered the driver and removed the launchers")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("installer", nargs="?")
    data_home = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
    ap.add_argument("--home", default=os.path.join(data_home, "rebocap-linux"))
    ap.add_argument("--com", default="COM5")
    ap.add_argument("--no-register", action="store_true")
    ap.add_argument("--uninstall", action="store_true")
    args = ap.parse_args()

    if args.uninstall:
        return uninstall()

    home = os.path.abspath(args.home)
    if args.installer and not os.path.isfile(args.installer):
        sys.exit(f"no such file: {args.installer}")

    if not args.installer and not os.path.isdir(os.path.join(home, "app")):
        sys.exit(__doc__)

    if subprocess.run(["pgrep", "-x", "vrserver"], capture_output=True).returncode == 0 and not args.no_register:
        sys.exit("SteamVR is running. Close it first (or it would overwrite the settings)")

    need("wine", "runs the Rebocap app")
    if args.installer:
        need("innoextract", "unpacks the vendor installer")

    need("cmake", "builds the driver")
    need("c++", "builds the driver")
    need("x86_64-w64-mingw32-gcc", "builds bridge.exe; package mingw-w64-gcc")
    os.makedirs(home, exist_ok=True)

    if args.installer:
        # Replaces the files that the installer ships. Files that the app created at run time
        # (device info, recordings, logs) are not in the installer and are not changed.
        print("1/5 unpacking the vendor app")
        run(["innoextract", "-s", "-d", home, args.installer]) # creates/updates <home>/app
    else:
        print("1/5 keeping the installed vendor app")

    vendor_driver = os.path.join(home, "app", "data", "rebocap_driver")
    if not os.path.isdir(vendor_driver):
        sys.exit("this installer doesn't have a data/rebocap_driver folder, unsupported version")

    print("2/5 building the bridge")
    run(["x86_64-w64-mingw32-gcc", "-O2", "-static", "-o", os.path.join(home, "bridge.exe"),
         os.path.join(SRC, "bridge", "bridge.c"), "-lws2_32"])

    print("3/5 building the SteamVR driver")
    build = os.path.join(home, "build")
    run(["cmake", "-S", os.path.join(SRC, "driver"), "-B", build, "-DCMAKE_BUILD_TYPE=Release"],
        stdout=subprocess.DEVNULL)
    run(["cmake", "--build", build, "-j", str(os.cpu_count() or 2)], stdout=subprocess.DEVNULL)
    driver = os.path.join(home, "driver", "rebocap")
    shutil.rmtree(driver, ignore_errors=True)
    os.makedirs(os.path.join(driver, "bin", "linux64"))
    for item in ("resources", "localization"):
        shutil.copytree(os.path.join(vendor_driver, item), os.path.join(driver, item))

    shutil.copy(os.path.join(vendor_driver, "driver.vrdrivermanifest"), driver)
    shutil.copytree(os.path.join(SRC, "driver", "resources", "settings"), os.path.join(driver, "resources", "settings"))
    shutil.copy(os.path.join(build, "driver_rebocap.so"), os.path.join(driver, "bin", "linux64"))

    print("4/5 preparing the Wine prefix")
    # The app does not need Mono or Gecko.
    env = dict(os.environ, WINEPREFIX=os.path.join(home, "pfx"), WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree,mshtml=")
    run(["wineboot", "-u"], env=env)
    with tempfile.NamedTemporaryFile("w", suffix=".reg", delete=False, newline="") as f:
        f.write(prefix_registry(args.com))

    run(["wine", "regedit", "/S", f.name], env=env)
    run(["wineserver", "-w"], env=env)
    os.remove(f.name)

    launcher = os.path.join(home, "rebocap.sh")
    with open(launcher, "w") as f:
        f.write(LAUNCHER.format(home=home, dongle=DONGLE))

    os.chmod(launcher, 0o755)
    shutil.copy(os.path.join(SRC, "driver", "configure.py"), os.path.join(home, "rebocap-config"))

    if args.no_register:
        print("5/5 skipped (--no-register)")
    else:
        print("5/5 registering with SteamVR and creating launchers")
        vrpathreg = os.path.join(steamvr_dir(), "bin", "vrpathreg.sh")
        # An older copy has the same driver name and would conflict.
        for path in registered_rebocap_drivers():
            run([vrpathreg, "removedriver", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        run([vrpathreg, "adddriver", driver], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        bin_launcher, bin_config, desktop = user_paths()
        for link, target in ((bin_launcher, launcher), (bin_config, os.path.join(home, "rebocap-config"))):
            os.makedirs(os.path.dirname(link), exist_ok=True)
            if os.path.lexists(link):
                os.remove(link)

            os.symlink(target, link)

        os.makedirs(os.path.dirname(desktop), exist_ok=True)
        with open(desktop, "w") as f:
            f.write(DESKTOP.format(launcher=launcher, icon=os.path.join(home, "app", "rebocap.ico")))

    print(f"\ninstalled in {home}")
    print("start the app with `rebocap` (or via desktop entry), then start SteamVR.")
    if not os.path.exists(DONGLE):
        print("the dongle is not plugged in right now")
    elif not os.access(DONGLE, os.R_OK | os.W_OK):
        group = __import__("grp").getgrgid(os.stat(DONGLE).st_gid).gr_name
        print(f"you cannot open the dongle: add yourself to the `{group}` group (sudo usermod -aG {group} $USER) "
              "and log in again.")

if __name__ == "__main__":
    main()
