# Rebocap app <-> SteamVR driver protocol

This document describes what a SteamVR driver must implement to work with the Rebocap desktop app.
The message schema is described by `proto/ReboTracker.proto`.
Rates and values marked *observed* come from sessions with app release v02 beta02.

## Transport

- The app uses the named pipe `\\.\pipe\Rebocap2048VRD`, the driver is the client, the pipe carries messages in both directions.
- It is used in message mode, and the driver side is non-blocking.
- One pipe message carries one protobuf message. It starts with a `uint32` little-endian payload length, which does not count these four bytes.
  A serialized `rebocap_pb.RebocapMsg` follows.
- The header and payload must be written with a single write.
- A payload is at most 2044 bytes.
- There is no version exchange and no keep-alive message.
- In this project, `bridge.exe` relays these messages over TCP. The same framing applies on the socket.

## Coordinate conventions

All poses exchanged with the app are in SteamVR's *standing* universe: metres, +Y up, -Z forward, +X right.
Quaternions are `[w, x, y, z]` (doubles).
Positions are `[x, y, z]` (floats).
A tracker with identity rotation faces forward.

SteamVR's raw tracking space differs from the standing universe by a translation `t` and a yaw angle.
The driver converts in both directions:

- headset pose sent to the app: `p' = Ry(-yaw) * (p + t)`, `q' = Ry(-yaw) * q`
- tracker poses received from the app submitted with
  `vecWorldFromDriverTranslation = -t` and `qWorldFromDriverRotation = Ry(yaw)`

`t` and `yaw` come from SteamVR's chaperone data.
Find the entry whose `universeID` equals the headset's `Prop_CurrentUniverseId_Uint64`.
`t` is its `standing.translation` and `yaw` is its `standing.yaw`.

The driver looks for the chaperone data in this order:

1. the headset's `Prop_DriverProvidedChaperoneJson_String`
2. the file pointed to by `Prop_DriverProvidedChaperonePath_String`
3. `chaperone_info.vrchap` in SteamVR's config directory

Steam Link publishes the data through the JSON property. It issues a new universe id on every reconnect and recentre.

## Driver -> app

| message                | content                                                                                            | when                                                                    |
|------------------------|----------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------|
| `tracker_status`       | `tracker_id = 0`, `status = OK` if the headset pose is valid and connected, else `DISCONNECTED`    | after connecting, and whenever that state changes                       |
| `uch` (UniverseChange) | `pos = t`, `yaw`                                                                                   | after connecting, when the play space is known, and whenever it changes |
| `path_failed`          | `failed = 1`: no chaperone file found. `failed = 2`: the universe id is not in the chaperone data. | while the play space cannot be determined, about once per 30 frames     |
| `position`             | `tracker_id = 0`, headset pose in the standing universe                                            | continuously, the app accepts at least 100 Hz (*observed*).             |

The handshake is `tracker_status` followed by `uch`. The driver repeats it after every reconnect.
The driver reports only the headset and does not report controllers.
The app places its body model at the headset pose, so the tracker positions that it sends depend on the driver's reported head position.

## App -> driver

In PC mode the app doesn't send anything.
In VR mode the app shows SteamVR as connected after a driver has connected and reported the headset.
Tracker output starts after calibration.

| message                 | content                                                                                    | rate (*observed*)                         |
|-------------------------|--------------------------------------------------------------------------------------------|-------------------------------------------|
| `tracker_added`         | `tracker_id`, `tracker_role`, `tracker_name = "rebocap_<role>"` for every assigned tracker | every 5 s, repeated for the whole session |
| `tracker_status`        | `status`, `battery_level` in 0..1                                                          | every 2 s per tracker                     |
| `position`              | pose of one tracker                                                                        | 60 Hz per tracker                         |
| `wip_setting`           | walk-in-place and hand options, see below                                                  | every 2 s                                 |
| `wip_info`              | walking speed and state                                                                    | while walk-in-place is on                 |
| `wip_func_state_change` | the "Walk-In-Place & Swap Pos" master switch changed                                       | on change                                 |

`tracker_id = tracker_role + 3` (*observed*). Roles:

| role    | body part              | SteamVR tracker role used by this driver |
|---------|------------------------|------------------------------------------|
| 0       | waist                  | `TrackerRole_Waist`                      |
| 1 / 2   | left / right upper leg | `TrackerRole_LeftKnee` / `RightKnee`     |
| 3 / 4   | left / right lower leg | `TrackerRole_LeftAnkle` / `RightAnkle`   |
| 5 / 6   | left / right foot      | `TrackerRole_LeftFoot` / `RightFoot`     |
| 7       | chest                  | `TrackerRole_Chest`                      |
| 8       | head                   | none                                     |
| 9 / 10  | left / right upper arm | `TrackerRole_LeftElbow` / `RightElbow`   |
| 11 / 12 | left / right lower arm | `TrackerRole_LeftWrist` / `RightWrist`   |

Positions come from the solved body model, they are NOT the raw sensor positions.
For example, a tracker that lies on a desk is still reported at the place where the model puts that body part.

### What the driver does with them

- `tracker_added` - register a `TrackedDeviceClass_GenericTracker` with serial `rebo_id_<id>` if  it does not exist yet.
  The message repeats periodically so all trackers eventually will be reported.
- `position` - `pos` is applied if it has 3 elements, `q` is applied if it has 4, `poseTimeOffset` and all velocities are zero.
- `tracker_status`:
  - `DISCONNECTED` - not connected, pose invalid
  - `OK` - connected, pose valid
  - `BUSY`, `ERROR`, `OCCLUDED` - connected, pose invalid
  - `battery_level` is written to `Prop_DeviceBatteryPercentage_Float` without scaling.
- If a tracker doesn't get a position or status for about 3 s, it is reported as disconnected until data arrives again.
- Positions for ids 16 (left hand) and 17 (right hand) are hand poses. See "Hand pose replacement" below.

## Walk-in-place

`wip_setting` fields:

| field                 | label in the app | meaning                                           |
|-----------------------|------------------|---------------------------------------------------|
| `open_wip`            | "Walk-In-Place"  | walk-in-place is on                               |
| `use_left_hand`       | "Ctrl Swap"      | selects the hand controller whose stick is driven |
| `joystick_control`    | "Stick Move"     | output only while the app reports walking         |
| `output_as_treadmill` | none             | false in all observed sessions                    |
| `replace_hand_pose`   | "Swap Ctrl Pos"  | see "Hand pose replacement"                       |

`wip_info` fields:

- `x` - walking speed. Clamp to 0..1.
- `wip_status` - `START_WALKING`, `WALKING`, `STOP_WALKING`, `STOPPED` (speed is zero), or `EXIST`.
- `y` and the embedded position are not needed.

Expected behaviour:

- Output is active while `open_wip` is set and either `joystick_control` is off or the app reports walking.
- The speed is written to the thumbstick of the selected hand controller, because games read locomotion only from the hand controllers.
- The stick points forward. If the user deflects the real stick by more than 0.15, the stick points in the real stick's direction.
- Touch is reported while the speed is above 0.03.
- If `wip_info` doesn't arrive for about 1 s, the speed is set to zero.
- If `output_as_treadmill` is set, the speed goes to the joystick of a separate controller with the treadmill role. Most games seem to not read that device.

## Hand pose replacement

If `replace_hand_pose` is set, the app can send `position` messages with `tracker_id` 16 and 17.
While they arrive, they replace the position and rotation of the matching real hand controller.
About 0.5 s after they stop, the controller's pose is used again.
In VR mode the app doesn't use lower arm or hand trackers, and it was not observed to send these ids.
