# Rebocap on Linux SteamVR

This project attempts to make Rebocap full body tracking work under Wine, and connects it to native Linux SteamVR.

Rebocap's Windows package has three parts:

The app runs under Wine without any changes.
This project replaces the application's steamvr DLL with a native driver that uses the same protocol.

- the GUI app, which talks to the USB dongle and does all pose estimation
- a SteamVR driver DLL
- a named pipe between them

```
dongle --/dev/ttyACM*--> rebocap.exe (Wine) <--named pipe--> bridge.exe (Wine) <--TCP--> driver_rebocap.so (vrserver)
```

The repository doesn't contain any files from the application itself. You'll need to download the Windows installer from Rebocap.
The install script unpacks it and copies the driver's icons and input profiles from it.

## Requirements

- SteamVR for Linux, Wine (tested with 11.15), `innoextract`
- Build tools: `cmake`, a C++17 compiler, `mingw-w64-gcc`, OpenVR headers (`openvr` package), `nlohmann-json`.
  The development tools in `tools/` and `driver/test/` also need `protoc` and the Python `protobuf` package.
- A static (non-variable) sans-serif font such as Noto Sans as Wine renders variable fonts badly.
- Permission to open the dongle's serial port. Your user must be in the group that owns `/dev/ttyACM*` (usually `uucp` on Arch-based systems, `dialout` on Debian-based systems).

## Install

Close SteamVR and run:

```
./install.py /path/to/rebocap_release_vXX.exe
```

The script installs the utils into `~/.local/share/rebocap-linux` (change with `--home DIR` if desired),
registers the driver with SteamVR, and creates a `rebocap` command with desktop entry.

- **Update after pulling changes:** run `./install.py` with no arguments.
  This will redo the entire install process for the Linux side of things without modifying the Rebocap install.
- **Newer vendor release:** run `./install.py` with the new installer.
  It replaces the app's program files and keeps the data that the app created (device info, recordings).
- **Uninstall:** `./install.py --uninstall` unregisters the driver and removes the launchers. Doesn't touch the Rebocap install.

Driver settings are stored in SteamVR's settings file.

***For safety do any firmware updates for the trackers and the receiver only on Windows. Do not do them under Wine.***

## Use

1. Plug in the dongle and run `rebocap`. This starts the app and the bridge.
2. Start SteamVR and connect the headset.
3. Choose VR mode in Rebocap. The SteamVR indicator turns green when the driver is connected.
   Calibrate with the headset on, the trackers should appear in SteamVR (same like)

You can start and restart the app and SteamVR in any order.

In PC mode the app sends nothing to SteamVR. The app behaves the same on Windows.

### Walk-in-place

Walk-in-place is supported. Enable "Walk-In-Place" in the app's "Walk-In-Place & Swap Pos" section.
The driver sets the walking speed to the thumbstick of the hand controller selected under "Ctrl Swap".
If you push the real stick while walking, it sets the direction. When you stop walking, the real stick works normally again.

By default the driver pushes the stick forward, and the game decides which direction that is.
To walk in the direction your hips face, close SteamVR and run:

```
rebocap-config wip_direction waist
rebocap-config wip_game_reference head         # or: controller, for games that move you where the hand points
```

`wip_game_reference` must match how the game interprets the stick.
If it is wrong, you'll walk at an angle that changes when you turn your head or hand.

### Settings

`rebocap-config` allows showing and changing settings for steamvr.vrsettings.

| key                          | default              | meaning                                                                                                          |
|------------------------------|----------------------|------------------------------------------------------------------------------------------------------------------|
| `bridge_host`, `bridge_port` | `127.0.0.1`, `36850` | where `bridge.exe` listens                                                                                       |
| `input_hooks`                | `true`               | lets walk-in-place and hand pose replacement change the hand controllers' input and poses (see "Things to know") |
| `wip_direction`              | `game`               | `game` or `waist`                                                                                                |
| `wip_game_reference`         | `head`               | `head` or `controller`. Used only with `waist`.                                                                  |
| `universe_fallback`          | `true`               | use raw tracking space if SteamVR's play space cannot be found                                                   |

### App on one machine, SteamVR on another

The bridge and the driver communicate over TCP, so the app (with the dongle) can run on a
different machine from SteamVR. This setup is not tested yet.

- On the app's machine, start the app with `BRIDGE_BIND=0.0.0.0 rebocap`.
- On the SteamVR machine, run `rebocap-config bridge_host <address of the app's machine>`.

The connection has no authentication and no encryption. Use it only on a network you trust.


Reverse engineered, analyzed, and written with some LLM assistance.
All functionality and code was verified and tested by a human with the actual hardware.
