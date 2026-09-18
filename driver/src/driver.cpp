// Linux SteamVR driver for Rebocap.
//
// The intention is to replace the vendor's Windows driver_rebocap.dll.
// This driver connects to the Rebocap app (running under Wine) through bridge.exe,
// sends the headset pose to the app, and adds the app's trackers to SteamVR.
// Protocol is described in docs/protocol.md.

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openvr_driver.h>

#include "input_hooks.hpp"
#include "net.hpp"
#include "pose_hooks.hpp"
#include "proto.hpp"

using Clock = std::chrono::steady_clock;

namespace {
    const char *kSettingsSection = "driver_rebocap";

    void Log(const std::string &msg) {
        vr::VRDriverLog()->Log(("rebocap: " + msg + "\n").c_str());
    }

    // Maps the app's role numbers (tracker_added.tracker_role) to the role names that SteamVR uses in its "trackers" settings.
    const char *SteamVrRole(const int role) {
        switch (role) {
            case 0:
                return "TrackerRole_Waist";
            case 1:
                return "TrackerRole_LeftKnee";
            case 2:
                return "TrackerRole_RightKnee";
            case 3:
                return "TrackerRole_LeftAnkle";
            case 4:
                return "TrackerRole_RightAnkle";
            case 5:
                return "TrackerRole_LeftFoot";
            case 6:
                return "TrackerRole_RightFoot";
            case 7:
                return "TrackerRole_Chest";
            case 9:
                return "TrackerRole_LeftElbow";
            case 10:
                return "TrackerRole_RightElbow";
            case 11:
                return "TrackerRole_LeftWrist";
            case 12:
                return "TrackerRole_RightWrist";
            default:
                return nullptr;
        }
    }

    // Origin of the standing universe in raw tracking space, from the chaperone data.
    struct Universe {
        uint64_t id = 0;
        double x = 0;
        double y = 0;
        double z = 0;
        double yaw = 0;
    };

    class Tracker final : public vr::ITrackedDeviceServerDriver {
    public:
        Tracker(const int id, const int role) : role_(role), serial_("rebo_id_" + std::to_string(id)) {
            pose_.poseTimeOffset = 0;
            pose_.qWorldFromDriverRotation = {.w = 1, .x = 0, .y = 0, .z = 0};
            pose_.qDriverFromHeadRotation = {.w = 1, .x = 0, .y = 0, .z = 0};
            pose_.qRotation = {.w = 1, .x = 0, .y = 0, .z = 0};
            pose_.result = vr::TrackingResult_Running_OK;
            pose_.poseIsValid = true;
            pose_.deviceIsConnected = true;
            last_message_ = Clock::now();
        }

        const std::string &serial() const {
            return serial_;
        }

        int role() const {
            return role_;
        }

        // Forward (-Z) axis projected onto the floor.
        // Returns false if the tracker is not tracking or the axis is almost vertical.
        bool FloorForward(double &fx, double &fz) const {
            if (!activated() || status_ != rebo::kOk || Clock::now() - last_message_ > std::chrono::milliseconds(500)) {
                return false;
            }

            const auto &q = pose_.qRotation;
            const double x = -2 * (q.x * q.z + q.w * q.y);
            const double z = -(1 - 2 * (q.x * q.x + q.y * q.y));

            const double len = std::hypot(x, z);
            if (len < 0.3) {
                return false;
            }

            fx = x / len;
            fz = z / len;

            return true;
        }

        bool activated() const {
            return index_ != vr::k_unTrackedDeviceIndexInvalid;
        }

        vr::EVRInitError Activate(const uint32_t index) override {
            index_ = index;
            auto *props = vr::VRProperties();
            const auto c = props->TrackedDeviceToPropertyContainer(index);

            props->SetStringProperty(c, vr::Prop_ModelNumber_String, "Rebocap Tracker");
            props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "rebocap");
            props->SetStringProperty(c, vr::Prop_RenderModelName_String, "{rebocap}/rendermodels/rebocap");
            props->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, true);
            props->SetBoolProperty(c, vr::Prop_DeviceCanPowerOff_Bool, false);
            props->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_GenericTracker);
            props->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_Invalid);

            // Prevents SteamVR from using a tracker as a hand controller.
            props->SetInt32Property(c, vr::Prop_ControllerHandSelectionPriority_Int32, -10000);
            props->SetStringProperty(c, vr::Prop_ControllerType_String, "vive_tracker");

            // The bindings file maps each SteamVR tracker role to a per-role input profile.
            props->SetStringProperty(c, vr::Prop_InputProfilePath_String,
                                     "{rebocap}/input/rebocap_tracker_bindings.json");

            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, "{rebocap}/icons/rebocap_off.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String, "{rebocap}/icons/rebocap_on.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearchingAlert_String,
                                     "{rebocap}/icons/rebocap_err.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, "{rebocap}/icons/rebocap_on.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReadyAlert_String,
                                     "{rebocap}/icons/rebocap_err.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceNotReady_String, "{rebocap}/icons/rebocap_err.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandby_String, "{rebocap}/icons/rebocap_off.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandbyAlert_String,
                                     "{rebocap}/icons/rebocap_off.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceAlertLow_String,
                                     "{rebocap}/icons/rebocap_low_pwr.png");

            if (const char *role = SteamVrRole(role_)) {
                vr::VRSettings()->SetString(vr::k_pch_Trackers_Section, ("/devices/rebocap/" + serial_).c_str(), role);
            }

            last_message_ = Clock::now();
            return vr::VRInitError_None;
        }

        void Deactivate() override {
            index_ = vr::k_unTrackedDeviceIndexInvalid;
        }

        void EnterStandby() override {}

        void *GetComponent(const char *) override {
            return nullptr;
        }

        void DebugRequest(const char *, char *response, const uint32_t size) override {
            if (size) {
                response[0] = 0;
            }
        }

        vr::DriverPose_t GetPose() override {
            return pose_;
        }

        void OnPosition(const rebo::Msg &m, const Universe *u) {
            if (!activated()) {
                return;
            }

            if (m.pos.size() == 3) {
                for (size_t i = 0; i < 3; i++) {
                    pose_.vecPosition[i] = m.pos[i];
                }
            }

            if (m.q.size() == 4) {
                pose_.qRotation = {m.q[0], m.q[1], m.q[2], m.q[3]};
            }

            if (timed_out_) {
                timed_out_ = false;
                status_ = rebo::kOk;
            }

            // Convert poses to raw tracking space.
            if (u) {
                pose_.vecWorldFromDriverTranslation[0] = -u->x;
                pose_.vecWorldFromDriverTranslation[1] = -u->y;
                pose_.vecWorldFromDriverTranslation[2] = -u->z;
                pose_.qWorldFromDriverRotation = {std::cos(u->yaw / 2), 0, std::sin(u->yaw / 2), 0};
            }

            ApplyStatus();
            Submit();
        }

        void OnStatus(const rebo::Msg &m) {
            if (!activated()) {
                return;
            }

            status_ = m.status;
            timed_out_ = false;
            if (const auto now = Clock::now(); status_ == rebo::kOk && now - last_battery_ > std::chrono::seconds(1)) {
                last_battery_ = now;
                const auto c = vr::VRProperties()->TrackedDeviceToPropertyContainer(index_);
                vr::VRProperties()->SetFloatProperty(c, vr::Prop_DeviceBatteryPercentage_Float, m.battery);
            }

            ApplyStatus();
            Submit();
        }

        // The app sends positions at 60 Hz and a status every 2 s. After 3 s without messages the
        // tracker is marked as disconnected.
        void CheckTimeout() {
            if (!activated() || Clock::now() - last_message_ < std::chrono::seconds(3)) {
                return;
            }

            status_ = rebo::kDisconnected;
            timed_out_ = true;

            ApplyStatus();
            Submit();
        }

    private:
        void ApplyStatus() {
            pose_.deviceIsConnected = status_ != rebo::kDisconnected;
            pose_.poseIsValid = status_ == rebo::kOk;
            pose_.result = status_ == rebo::kOk ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
        }

        void Submit() {
            last_message_ = Clock::now();
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index_, pose_, sizeof pose_);
        }

        int role_;
        std::string serial_;
        uint32_t index_ = vr::k_unTrackedDeviceIndexInvalid;
        int status_ = rebo::kOk;
        bool timed_out_ = false;
        vr::DriverPose_t pose_{};
        Clock::time_point last_message_;
        Clock::time_point last_battery_;
    };

    // Walk-in-place output as a separate controller with the treadmill role.
    // SteamVR merges its joystick into locomotion bindings.
    // Used if the app asks for it or input hooks are not installed (see WipUsesTreadmill).
    class Treadmill final : public vr::ITrackedDeviceServerDriver {
    public:
        Treadmill() {
            pose_.qWorldFromDriverRotation = {.w = 1, .x = 0, .y = 0, .z = 0};
            pose_.qDriverFromHeadRotation = {.w = 1, .x = 0, .y = 0, .z = 0};
            pose_.qRotation = {.w = 1, .x = 0, .y = 0, .z = 0};
        }

        static constexpr const char *kSerial = "rebocap_wip_virtual_controller";
        bool activated() const {
            return index_ != vr::k_unTrackedDeviceIndexInvalid;
        }

        vr::EVRInitError Activate(const uint32_t index) override {
            index_ = index;
            auto *props = vr::VRProperties();
            const auto c = props->TrackedDeviceToPropertyContainer(index);

            props->SetStringProperty(c, vr::Prop_ModelNumber_String, "Rebocap Walk-in-Place");
            props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "rebocap");
            props->SetStringProperty(c, vr::Prop_RenderModelName_String, "vr_controller_vive_1_5");
            props->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, false);
            props->SetBoolProperty(c, vr::Prop_DeviceIsWireless_Bool, true);
            props->SetBoolProperty(c, vr::Prop_HasControllerComponent_Bool, true);
            props->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_Treadmill);
            props->SetInt32Property(c, vr::Prop_ControllerHandSelectionPriority_Int32, -1);

            // Must match controller_type in wip_profile.json. SteamVR selects bindings using this name.
            props->SetStringProperty(c, vr::Prop_ControllerType_String, "wip_rebocap_controller");
            props->SetInt32Property(c, vr::Prop_Axis0Type_Int32, vr::k_eControllerAxis_TrackPad);
            props->SetInt32Property(c, vr::Prop_Axis1Type_Int32, vr::k_eControllerAxis_Trigger);
            props->SetInt32Property(c, vr::Prop_Axis2Type_Int32, vr::k_eControllerAxis_Joystick);
            props->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{rebocap}/input/wip_profile.json");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String,
                                     "{rebocap}/icons/rebocap_virtual_treadmill_off.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String,
                                     "{rebocap}/icons/rebocap_virtual_treadmill_on.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceNotReady_String,
                                     "{rebocap}/icons/rebocap_virtual_treadmill_err.png");

            auto *input = vr::VRDriverInput();
            vr::VRInputComponentHandle_t unused;

            // The same components as the vendor's controller, so the vendor's input profile applies.
            for (const char *name :
                 {"/input/trackpad/click", "/input/trackpad/touch", "/input/joystick/click", "/input/trigger/click",
                  "/input/trigger/touch", "/input/grip/click", "/input/grip/touch"}) {
                input->CreateBooleanComponent(c, name, &unused);
            }

            for (const char *name : {"/input/trackpad/x", "/input/trackpad/y"}) {
                input->CreateScalarComponent(c, name, &unused, vr::VRScalarType_Absolute,
                                             vr::VRScalarUnits_NormalizedTwoSided);
            }

            input->CreateScalarComponent(c, "/input/trigger/x", &unused, vr::VRScalarType_Absolute,
                                         vr::VRScalarUnits_NormalizedOneSided);
            input->CreateBooleanComponent(c, "/input/joystick/touch", &touch_);
            input->CreateScalarComponent(c, "/input/joystick/x", &x_, vr::VRScalarType_Absolute,
                                         vr::VRScalarUnits_NormalizedTwoSided);
            input->CreateScalarComponent(c, "/input/joystick/y", &y_, vr::VRScalarType_Absolute,
                                         vr::VRScalarUnits_NormalizedTwoSided);

            return vr::VRInitError_None;
        }

        void Deactivate() override {
            index_ = vr::k_unTrackedDeviceIndexInvalid;
        }

        void EnterStandby() override {}

        void *GetComponent(const char *) override {
            return nullptr;
        }

        void DebugRequest(const char *, char *response, const uint32_t size) override {
            if (size) {
                response[0] = 0;
            }
        }

        vr::DriverPose_t GetPose() override {
            return pose_;
        }

        void SetConnected(bool connected) {
            if (!activated()) {
                return;
            }

            pose_.deviceIsConnected = connected;
            pose_.poseIsValid = connected;
            pose_.result = connected ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index_, pose_, sizeof pose_);

            if (!connected) {
                SetStick(0, 0);
            }
        }

        void SetStick(const float x, const float y) {
            if (!activated() || (x == last_x_ && y == last_y_)) {
                return;
            }

            last_x_ = x;
            last_y_ = y;

            auto *input = vr::VRDriverInput();
            input->UpdateScalarComponent(x_, x, 0);
            input->UpdateScalarComponent(y_, y, 0);
            input->UpdateBooleanComponent(touch_, std::hypot(x, y) > 0.03f, 0);
        }

    private:
        uint32_t index_ = vr::k_unTrackedDeviceIndexInvalid;

        vr::DriverPose_t pose_{};

        vr::VRInputComponentHandle_t x_ = 0;
        vr::VRInputComponentHandle_t y_ = 0;
        vr::VRInputComponentHandle_t touch_ = 0;

        float last_x_ = 0;
        float last_y_ = 0;
    };

    class Provider final : public vr::IServerTrackedDeviceProvider {
    public:
        vr::EVRInitError Init(vr::IVRDriverContext *context) override {
            VR_INIT_SERVER_DRIVER_CONTEXT(context);

            char host[256] = "127.0.0.1";

            vr::EVRSettingsError err = vr::VRSettingsError_None;
            vr::VRSettings()->GetString(kSettingsSection, "bridge_host", host, sizeof host, &err);

            if (err != vr::VRSettingsError_None) {
                std::snprintf(host, sizeof host, "127.0.0.1");
            }

            int port = vr::VRSettings()->GetInt32(kSettingsSection, "bridge_port", &err);
            if (err != vr::VRSettingsError_None || port <= 0) {
                port = 36850;
            }

            universe_fallback_ = vr::VRSettings()->GetBool(kSettingsSection, "universe_fallback", &err);
            if (err != vr::VRSettingsError_None) {
                universe_fallback_ = true;
            }

            bool want_hooks = vr::VRSettings()->GetBool(kSettingsSection, "input_hooks", &err);
            if (err != vr::VRSettingsError_None) {
                want_hooks = true;
            }

            if (want_hooks) {
                bool ok = rebo::hooks::Install(vr::VRDriverInput());
                Log(ok ? "input hooks installed (walk-in-place can drive the hand controller's stick)"
                       : "could not install input hooks, walk-in-place is limited to the treadmill controller");
                ok = rebo::pose_hooks::Install(vr::VRServerDriverHost());
                Log(ok ? "pose hook installed (hand pose replacement available)"
                       : "could not install the pose hook, hand pose replacement is unavailable");
            }

            char text[64] = "";

            vr::VRSettings()->GetString(kSettingsSection, "wip_direction", text, sizeof text, &err);
            wip_waist_direction_ = err == vr::VRSettingsError_None && std::string(text) == "waist";

            vr::VRSettings()->GetString(kSettingsSection, "wip_game_reference", text, sizeof text, &err);
            wip_reference_controller_ = err == vr::VRSettingsError_None && std::string(text) == "controller";

            Log(std::string("walk-in-place direction: ") +
                (!wip_waist_direction_       ? "decided by the game"
                 : wip_reference_controller_ ? "waist tracker (game moves relative to the controller)"
                                             : "waist tracker (game moves relative to the head)"));

            default_chaperone_ = FindDefaultChaperonePath();
            Log("default chaperone file: " +
                (default_chaperone_.empty() ? std::string("not found") : default_chaperone_));
            client_.start(host, port, Log);
            return vr::VRInitError_None;
        }

        void Cleanup() override {
            rebo::hooks::Remove();
            rebo::pose_hooks::Remove();
            client_.stop();
            VR_CLEANUP_SERVER_DRIVER_CONTEXT();
        }

        const char *const *GetInterfaceVersions() override {
            return vr::k_InterfaceVersions;
        }

        bool ShouldBlockStandbyMode() override {
            return false;
        }

        void EnterStandby() override {}

        void LeaveStandby() override {}

        void RunFrame() override {
            vr::VREvent_t ev;
            while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof ev)) {
            }

            auto now = Clock::now();
            if (!client_.connected()) {
                UpdateWalkInPlace(now); // releases the stick
                if (now - last_timeout_check_ > std::chrono::milliseconds(200)) {
                    last_timeout_check_ = now;
                    for (auto &t : trackers_) {
                        t.second->CheckTimeout();
                    }
                }

                return;
            }

            if (client_.generation() != generation_) {
                generation_ = client_.generation();
                hello_sent_ = false;
            }

            client_.drain(inbox_);
            const Universe *u = have_universe_ ? &universe_ : nullptr;
            for (const rebo::Msg &m : inbox_) {
                switch (m.kind) {
                    case rebo::kTrackerAdded:
                        AddTracker(m);
                        break;
                    case rebo::kPosition:
                        if (m.tracker_id == 16 || m.tracker_id == 17) {
                            OnHandPose(m, u);
                        } else if (auto it = trackers_.find(m.tracker_id); it != trackers_.end()) {
                            it->second->OnPosition(m, u);
                        } else {
                            NoteUnknownId(m.tracker_id, now);
                        }
                        break;
                    case rebo::kTrackerStatus:
                        if (auto it = trackers_.find(m.tracker_id); it != trackers_.end()) {
                            it->second->OnStatus(m);
                        }
                        break;
                    case rebo::kWipSetting:
                        OnWipSetting(m);
                        break;
                    case rebo::kWipInfo:
                        OnWipInfo(m, now);
                        break;
                    default:
                        break; // wip_func_state_change tells the vendor driver to reload its hook switch. Not needed here.
                }
            }

            inbox_.clear();

            if (now - last_timeout_check_ > std::chrono::milliseconds(200)) {
                last_timeout_check_ = now;
                for (auto &t : trackers_) {
                    t.second->CheckTimeout();
                }
            }

            UpdateWalkInPlace(now);

            vr::TrackedDevicePose_t hmd;
            vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0, &hmd, 1);
            bool hmd_ok = hmd.bPoseIsValid && hmd.bDeviceIsConnected;

            UpdateUniverse(now);

            if (!hello_sent_ || hmd_ok != last_hmd_ok_) {
                client_.send(rebo::encode_status(0, hmd_ok ? rebo::kOk : rebo::kDisconnected));
                last_hmd_ok_ = hmd_ok;

                if (have_universe_) {
                    hello_sent_ = SendUniverse();
                }
            }

            // The vendor driver sends the headset pose on every vrserver frame.
            // 100 Hz is enough for the app's 60 Hz solver and reduces traffic when the bridge is on another machine.
            if (now - last_hmd_send_ >= std::chrono::milliseconds(10)) {
                last_hmd_send_ = now;
                SendHmdPose(hmd);
            }
        }

    private:
        // Positions can arrive before the app's next tracker_added (sent every 5 s).
        // An id is logged only if it is still unannounced after 10 s.
        void NoteUnknownId(const int id, Clock::time_point now) {
            if (unknown_ids_.size() >= 32 && !unknown_ids_.count(id)) {
                return;
            }

            auto &entry = unknown_ids_.try_emplace(id, now, false).first->second;
            if (entry.second || now - entry.first < std::chrono::seconds(10)) {
                return;
            }

            entry.second = true;
            Log("ignoring positions for tracker id " + std::to_string(id) + ", which has not yet been announced");
        }

        // Ids 16 (left) and 17 (right) are hand poses. They replace the poses of the real hand controllers.
        void OnHandPose(const rebo::Msg &m, const Universe *u) {
            const int hand = m.tracker_id - 16;
            if (!hand_pose_seen_[hand]) {
                hand_pose_seen_[hand] = true;
                Log(std::string("the app is sending a ") + (hand == 0 ? "left" : "right") + " hand pose");
            }

            rebo::pose_hooks::Hand &h = rebo::pose_hooks::g.hands[hand];

            std::lock_guard lock(h.mutex);

            if (m.pos.size() == 3) {
                for (size_t i = 0; i < 3; i++) {
                    h.position[i] = m.pos[i];
                }
                h.have_position = true;
            }

            if (m.q.size() == 4) {
                h.rotation = {.w = m.q[0], .x = m.q[1], .y = m.q[2], .z = m.q[3]};
                h.have_rotation = true;
            }

            if (u) {
                h.world_translation[0] = -u->x;
                h.world_translation[1] = -u->y;
                h.world_translation[2] = -u->z;
                h.world_rotation = {.w = std::cos(u->yaw / 2), .x = 0, .y = std::sin(u->yaw / 2), .z = 0};
            }

            h.updated = Clock::now();
        }

        // The pose hook needs the device indices of the real hand controllers.
        static void FindHandControllers() {
            auto *props = vr::VRProperties();
            uint32_t found[2] = {vr::k_unTrackedDeviceIndexInvalid, vr::k_unTrackedDeviceIndexInvalid};

            for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
                const auto c = props->TrackedDeviceToPropertyContainer(i);

                vr::ETrackedPropertyError err = vr::TrackedProp_Success;
                if (props->GetInt32Property(c, vr::Prop_DeviceClass_Int32, &err) != vr::TrackedDeviceClass_Controller ||
                    err != vr::TrackedProp_Success) {
                    continue;
                }

                const int32_t role = props->GetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, &err);
                if (err != vr::TrackedProp_Success) {
                    continue;
                }

                if (role == vr::TrackedControllerRole_LeftHand && found[0] == vr::k_unTrackedDeviceIndexInvalid) {
                    found[0] = i;
                }

                if (role == vr::TrackedControllerRole_RightHand && found[1] == vr::k_unTrackedDeviceIndexInvalid) {
                    found[1] = i;
                }
            }

            for (int hand = 0; hand < 2; hand++) {
                auto &slot = rebo::pose_hooks::g.hands[hand].device_index;
                if (slot.load() == found[hand]) {
                    continue;
                }

                slot.store(found[hand]);
                if (found[hand] != vr::k_unTrackedDeviceIndexInvalid) {
                    Log(std::string(hand == 0 ? "left" : "right") + " hand controller is device " +
                        std::to_string(found[hand]));
                }
            }
        }

        // Selects the walk-in-place output.
        // Most games read only the controller's stick, so we use injection by default.
        // The treadmill device is used if the app asks for it or the hooks are not installed.
        bool WipUsesTreadmill() const {
            return wip_as_treadmill_ || !rebo::hooks::g.installed;
        }

        // The app repeats wip_setting every 2 s, so unchanged settings are ignored.
        void OnWipSetting(const rebo::Msg &m) {
            wip_joystick_control_ = m.joystick_control;
            wip_hand_ = m.use_left_hand ? 0 : 1;
            if (rebo::pose_hooks::g.installed &&
                rebo::pose_hooks::g.enabled.exchange(m.replace_hand_pose) != m.replace_hand_pose) {
                Log(std::string("hand pose replacement ") + (m.replace_hand_pose ? "enabled" : "disabled") +
                    " by the app");
            }

            if (m.open_wip == wip_open_ && m.output_as_treadmill == wip_as_treadmill_) {
                return;
            }

            wip_open_ = m.open_wip;
            wip_as_treadmill_ = m.output_as_treadmill;
            wip_speed_ = 0;
            wip_walking_ = false;

            rebo::hooks::Drive(-1, 0);
            const bool treadmill = wip_open_ && WipUsesTreadmill();

            Log(std::string("walk-in-place ") + (!wip_open_       ? "disabled"
                                                 : treadmill      ? "enabled, output: treadmill controller"
                                                 : wip_hand_ == 0 ? "enabled, output: left hand stick"
                                                                  : "enabled, output: right hand stick"));

            if (treadmill && !treadmill_) {
                auto t = std::make_unique<Treadmill>();
                if (vr::VRServerDriverHost()->TrackedDeviceAdded(Treadmill::kSerial, vr::TrackedDeviceClass_Controller,
                                                                 t.get())) {
                    treadmill_ = std::move(t);
                } else {
                    Log("SteamVR rejected the walk-in-place controller");
                }
            }

            if (treadmill_) {
                treadmill_->SetConnected(treadmill);
            }
        }

        void OnWipInfo(const rebo::Msg &m, Clock::time_point now) {
            last_wip_info_ = now;
            wip_speed_ = std::fmin(std::fmax(m.wip_x, 0.0f), 1.0f);

            if (m.wip_status == rebo::kWipWalking) {
                wip_walking_ = true;
            }

            if (m.wip_status == rebo::kWipStopWalking || m.wip_status == rebo::kWipStopped) {
                wip_walking_ = false;
            }

            if (m.wip_status == rebo::kWipStopped) {
                wip_speed_ = 0;
            }
        }

        void UpdateWalkInPlace(Clock::time_point now) {
            if (rebo::hooks::g.installed && now - last_hook_resolve_ > std::chrono::seconds(1)) {
                last_hook_resolve_ = now;
                if (rebo::hooks::ResolvePending()) {
                    Log("captured a hand controller's stick for walk-in-place");
                }

                if (rebo::pose_hooks::g.installed) {
                    FindHandControllers();
                }

                if (!hooks_seen_foreign_ && rebo::hooks::g.foreign_components > 0) {
                    hooks_seen_foreign_ = true;
                    Log("input hooks are seeing components created by other drivers");
                }
            }

            if (now - last_wip_output_ < std::chrono::milliseconds(10)) {
                return;
            }

            last_wip_output_ = now;

            // Stop walking if the app stops sending wip_info.
            if (now - last_wip_info_ > std::chrono::seconds(1)) {
                wip_speed_ = 0;
            }

            const bool output = wip_open_ && client_.connected() && (!wip_joystick_control_ || wip_walking_);
            const float speed = output && wip_speed_ > 0.03f ? wip_speed_ : 0;
            if (WipUsesTreadmill()) {
                if (treadmill_) {
                    treadmill_->SetStick(0, speed);
                }

                return;
            }

            float dir_x = 0;
            float dir_y = 1;

            if (speed > 0 && wip_waist_direction_) {
                WaistStickDirection(dir_x, dir_y);
            }

            rebo::hooks::Drive(wip_hand_, speed, dir_x, dir_y);
        }

        // A game moves the player along its reference direction (head or controller), rotated by the stick.
        // This computes the stick direction that moves the player where the waist tracker faces.
        // If an input is missing, the direction stays (0, 1) - forward.
        void WaistStickDirection(float &dir_x, float &dir_y) const {
            double wx = 0;
            double wz = 0;
            bool have_waist = false;
            for (const auto &t : trackers_) {
                if (t.second->role() == 0 && t.second->FloorForward(wx, wz)) {
                    have_waist = true;
                    break;
                }
            }

            if (!have_waist) {
                return;
            }

            uint32_t index = vr::k_unTrackedDeviceIndex_Hmd;
            if (wip_reference_controller_) {
                const auto container = rebo::hooks::g.sticks[wip_hand_].container.load();

                index = vr::k_unTrackedDeviceIndexInvalid;
                for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
                    if (vr::VRProperties()->TrackedDeviceToPropertyContainer(i) == container) {
                        index = i;
                        break;
                    }
                }

                if (index == vr::k_unTrackedDeviceIndexInvalid) {
                    return;
                }
            }

            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
            vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0, poses, index + 1);
            if (!poses[index].bPoseIsValid) {
                return;
            }

            const auto &m = poses[index].mDeviceToAbsoluteTracking.m;
            double rx = -m[0][2], rz = -m[2][2]; // the device's -Z axis in raw tracking space
            if (have_universe_) {
                // raw tracking space -> standing frame
                const double a = -universe_.yaw;
                const double c = std::cos(a);
                const double s = std::sin(a);
                const double x = rx * c + rz * s;
                const double z = -rx * s + rz * c;
                rx = x;
                rz = z;
            }

            const double len = std::hypot(rx, rz);
            if (len < 0.3) {
                return; // pointing almost straight up or down
            }

            rx /= len;
            rz /= len;

            // The reference right vector is (-rz, rx). Project the waist direction onto right and forward.
            dir_x = static_cast<float>(wx * -rz + wz * rx);
            dir_y = static_cast<float>(wx * rx + wz * rz);
        }

        void AddTracker(const rebo::Msg &m) {
            if (auto it = trackers_.find(m.tracker_id); it != trackers_.end()) {
                return; // the app repeats tracker_added every 5 s
            }

            auto tracker = std::make_unique<Tracker>(m.tracker_id, m.role);

            if (!vr::VRServerDriverHost()->TrackedDeviceAdded(tracker->serial().c_str(),
                                                              vr::TrackedDeviceClass_GenericTracker, tracker.get())) {
                Log("SteamVR rejected tracker " + tracker->serial());
                return;
            }

            Log("added tracker " + tracker->serial() + " (" + m.name + "), role " + std::to_string(m.role));
            trackers_[m.tracker_id] = std::move(tracker);
        }

        static std::string FindDefaultChaperonePath() {
            std::string reg;
            if (const char *o = std::getenv("VR_PATHREG_OVERRIDE")) {
                reg = o;
            } else if (const char *x = std::getenv("XDG_CONFIG_HOME")) {
                reg = std::string(x) + "/openvr/openvrpaths.vrpath";
            } else if (const char *h = std::getenv("HOME")) {
                reg = std::string(h) + "/.config/openvr/openvrpaths.vrpath";
            }

            std::ifstream f(reg);
            auto j = nlohmann::json::parse(f, nullptr, false);
            if (!j.is_object()) {
                return "";
            }

            auto config = j.find("config");
            if (config == j.end() || !config->is_array()) {
                return "";
            }

            for (const auto &c : *config) {
                if (!c.is_string()) {
                    continue;
                }

                if (std::string path = c.get<std::string>() + "/chaperone_info.vrchap"; std::ifstream(path).good()) {
                    return path;
                }
            }
            return "";
        }

        // Chaperone data has the same structure in a .vrchap file and in the headset driver's JSON property.
        // Heavyhanded type checks are needed because anything that throws here would crash SteamVR.
        static bool SearchUniverse(const nlohmann::json &j, uint64_t id, Universe &out) {
            if (!j.is_object()) {
                return false;
            }

            const auto universes = j.find("universes");
            if (universes == j.end() || !universes->is_array()) {
                return false;
            }

            for (const auto &u : *universes) {
                if (!u.is_object()) {
                    continue;
                }

                auto uid = u.find("universeID");
                if (uid == u.end()) {
                    continue;
                }

                uint64_t v = 0;
                if (uid->is_string()) {
                    v = std::strtoull(uid->get_ref<const std::string &>().c_str(), nullptr, 10);
                } else if (uid->is_number_unsigned()) {
                    v = uid->get<uint64_t>();
                } else if (uid->is_number_integer()) {
                    v = static_cast<uint64_t>(uid->get<int64_t>());
                }

                if (v != id) {
                    continue;
                }

                const auto s = u.find("standing");
                if (s == u.end() || !s->is_object()) {
                    return false;
                }

                const auto t = s->find("translation");
                if (t == s->end() || !t->is_array() || t->size() != 3) {
                    return false;
                }

                for (const auto &e : *t) {
                    if (!e.is_number()) {
                        return false;
                    }
                }

                out.id = id;
                out.x = (*t)[0].get<double>();
                out.y = (*t)[1].get<double>();
                out.z = (*t)[2].get<double>();

                const auto yaw = s->find("yaw");
                out.yaw = yaw != s->end() && yaw->is_number() ? yaw->get<double>() : 0.0;

                return true;
            }

            return false;
        }

        static bool SearchUniverseText(const std::string &text, const uint64_t id, Universe &out) {
            if (text.empty()) {
                return false;
            }

            return SearchUniverse(nlohmann::json::parse(text, nullptr, false), id, out);
        }

        static bool SearchUniverseFile(const std::string &path, const uint64_t id, Universe &out) {
            if (path.empty()) {
                return false;
            }

            std::ifstream f(path);
            if (!f.good()) {
                return false;
            }

            return SearchUniverse(nlohmann::json::parse(f, nullptr, false), id, out);
        }

        void UpdateUniverse(const Clock::time_point now) {
            auto *props = vr::VRProperties();
            const auto hmd = props->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd);
            vr::ETrackedPropertyError perr = vr::TrackedProp_Success;
            const uint64_t id = props->GetUint64Property(hmd, vr::Prop_CurrentUniverseId_Uint64, &perr);

            if (perr != vr::TrackedProp_Success) {
                return; // no headset yet
            }

            if (have_universe_ && !using_fallback_ && universe_.id == id) {
                return;
            }

            if (now - last_universe_search_ < std::chrono::seconds(1)) {
                return;
            }

            last_universe_search_ = now;

            // Streaming headset drivers (Steam Link) publish the play space as a JSON property without writing chaperone_info.vrchap.
            // Check the property first.
            Universe found;
            bool ok = false;
            try {
                ok =
                    SearchUniverseText(
                        props->GetStringProperty(hmd, vr::Prop_DriverProvidedChaperoneJson_String, &perr), id, found) ||
                    SearchUniverseFile(
                        props->GetStringProperty(hmd, vr::Prop_DriverProvidedChaperonePath_String, &perr), id, found) ||
                    SearchUniverseFile(default_chaperone_, id, found);
            } catch (const std::exception &e) {
                Log(std::string("chaperone lookup failed: ") + e.what());
            }

            if (ok) {
                universe_ = found;
                have_universe_ = true;
                using_fallback_ = false;
                universe_failures_ = 0;
                Log("universe " + std::to_string(id) + ": translation " + std::to_string(found.x) + " " +
                    std::to_string(found.y) + " " + std::to_string(found.z) + ", yaw " + std::to_string(found.yaw));
                SendUniverse();
                return;
            }

            if (using_fallback_) {
                return;
            }

            if (universe_failures_++ % 30 == 0) {
                client_.send(rebo::encode_path_failed(default_chaperone_.empty() ? 1 : 2));
            }

            // Without a universe the app doesn't enable VR mode.
            // The fallback is raw tracking space with a zero offset.
            // This is consistent because tracker poses return in the same frame as the headset pose that was sent.
            // The floor is at y = 0 only if raw space has it there.
            if (universe_fallback_ && universe_failures_ >= 3) {
                Log("universe " + std::to_string(id) + " not found in any chaperone file; using raw tracking space");
                universe_ = Universe{};
                universe_.id = id;
                have_universe_ = true;
                using_fallback_ = true;
                SendUniverse();
            }
        }

        bool SendUniverse() {
            const float pos[3] = {static_cast<float>(universe_.x), static_cast<float>(universe_.y), static_cast<float>(universe_.z)};
            return client_.send(rebo::encode_universe(static_cast<float>(universe_.yaw), pos));
        }

        void SendHmdPose(const vr::TrackedDevicePose_t &hmd) {
            const auto &m = hmd.mDeviceToAbsoluteTracking.m;
            double q[4];
            q[0] = std::sqrt(std::fmax(0, 1 + m[0][0] + m[1][1] + m[2][2])) / 2;
            q[1] = std::copysign(std::sqrt(std::fmax(0, 1 + m[0][0] - m[1][1] - m[2][2])) / 2, m[2][1] - m[1][2]);
            q[2] = std::copysign(std::sqrt(std::fmax(0, 1 - m[0][0] + m[1][1] - m[2][2])) / 2, m[0][2] - m[2][0]);
            q[3] = std::copysign(std::sqrt(std::fmax(0, 1 - m[0][0] - m[1][1] + m[2][2])) / 2, m[1][0] - m[0][1]);

            double p[3] = {m[0][3], m[1][3], m[2][3]};

            if (have_universe_) {
                // Raw tracking space -> standing universe : translate, then rotate by -yaw about Y.
                p[0] += universe_.x;
                p[1] += universe_.y;
                p[2] += universe_.z;

                const double a = -universe_.yaw;
                const double c = std::cos(a);
                const double s = std::sin(a);
                const double x = p[0] * c + p[2] * s;
                const double z = -p[0] * s + p[2] * c;
                p[0] = x;
                p[2] = z;

                const double rw = std::cos(a / 2);
                const double ry = std::sin(a / 2);
                const double w = rw * q[0] - ry * q[2];
                const double qx = rw * q[1] + ry * q[3];
                const double qy = rw * q[2] + ry * q[0];
                const double qz = rw * q[3] - ry * q[1];
                q[0] = w;
                q[1] = qx;
                q[2] = qy;
                q[3] = qz;
            }

            const float pos[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
            client_.send(rebo::encode_position(0, q, pos));
        }

        rebo::BridgeClient client_;
        std::vector<rebo::Msg> inbox_;
        std::map<int, std::unique_ptr<Tracker>> trackers_;
        std::unique_ptr<Treadmill> treadmill_;
        bool wip_open_ = false;
        bool wip_as_treadmill_ = false;
        bool wip_joystick_control_ = false;
        bool wip_walking_ = false;
        bool hooks_seen_foreign_ = false;
        bool hand_pose_seen_[2] = {false, false};
        std::map<int, std::pair<Clock::time_point, bool>> unknown_ids_; // first seen, already reported
        bool wip_waist_direction_ = false;
        bool wip_reference_controller_ = false;
        int wip_hand_ = 0;
        float wip_speed_ = 0;
        Clock::time_point last_wip_info_;
        Clock::time_point last_wip_output_;
        Clock::time_point last_hook_resolve_;
        unsigned generation_ = 0;
        bool hello_sent_ = false;
        bool last_hmd_ok_ = false;
        bool have_universe_ = false;
        bool using_fallback_ = false;
        bool universe_fallback_ = true;
        int universe_failures_ = 0;
        Universe universe_;
        std::string default_chaperone_;
        Clock::time_point last_hmd_send_;
        Clock::time_point last_timeout_check_;
        Clock::time_point last_universe_search_;
    };

    Provider g_provider;
} // namespace

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *interface_name, int *return_code) {
    if (std::string(interface_name) == vr::IServerTrackedDeviceProvider_Version) {
        return &g_provider;
    }

    if (return_code) {
        *return_code = vr::VRInitError_Init_InterfaceNotFound;
    }

    return nullptr;
}
