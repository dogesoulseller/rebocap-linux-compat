// Hand pose replacement ("Swap Ctrl Pos" in the UI).
// The app sends hand poses as tracker ids 16 (left) and 17 (right).
// While they arrive, they replace the real hand controller's pose updates.
// The hook is on IVRServerDriverHost::TrackedDevicePoseUpdated, which the controller's driver calls.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

#include <openvr_driver.h>

#include "vtable.hpp"

namespace rebo::pose_hooks {
    constexpr int kPoseUpdatedSlot = 1; // TrackedDeviceAdded = 0, TrackedDevicePoseUpdated = 1
    using PoseUpdatedFn = void (*)(vr::IVRServerDriverHost *, uint32_t, const vr::DriverPose_t &, uint32_t);
    using Clock = std::chrono::steady_clock;

    struct Hand {
        std::atomic<uint32_t> device_index{vr::k_unTrackedDeviceIndexInvalid};
        std::mutex mutex; // guards the fields below
        bool have_position = false;
        bool have_rotation = false;
        double position[3] = {};
        vr::HmdQuaternion_t rotation{.w = 1, .x = 0, .y = 0, .z = 0};
        double world_translation[3] = {};
        vr::HmdQuaternion_t world_rotation{.w = 1, .x = 0, .y = 0, .z = 0};
        Clock::time_point updated;
    };

    struct State {
        void **vtable = nullptr;
        void *original = nullptr;
        bool installed = false;
        std::atomic<bool> enabled{false}; // replace_hand_pose
        Hand hands[2]; // 0 = left, 1 = right
    };

    inline State g;

    inline void HookPoseUpdated(vr::IVRServerDriverHost *self, const uint32_t index, const vr::DriverPose_t &pose,
                                const uint32_t size) {
        const auto original = reinterpret_cast<PoseUpdatedFn>(g.original);

        if (g.enabled.load() && size == sizeof(vr::DriverPose_t)) {
            for (Hand &h: g.hands) {
                if (index != h.device_index.load()) {
                    continue;
                }

                std::lock_guard lock(h.mutex);

                // The app stopped sending - pass the real pose through.
                if (Clock::now() - h.updated > std::chrono::milliseconds(500)) {
                    break;
                }

                vr::DriverPose_t replaced = pose;
                if (h.have_position) {
                    for (int i = 0; i < 3; i++) {
                        replaced.vecPosition[i] = h.position[i];
                    }
                }

                if (h.have_rotation) {
                    replaced.qRotation = h.rotation;
                }

                // The app's coordinates are in the standing frame, the same as for trackers.
                for (int i = 0; i < 3; i++) {
                    replaced.vecWorldFromDriverTranslation[i] = h.world_translation[i];
                }

                replaced.qWorldFromDriverRotation = h.world_rotation;

                // The real controller's velocities do not apply to the replaced pose.
                for (int i = 0; i < 3; i++) {
                    replaced.vecVelocity[i] = 0.0;
                    replaced.vecAcceleration[i] = 0.0;
                    replaced.vecAngularVelocity[i] = 0.0;
                    replaced.vecAngularAcceleration[i] = 0.0;
                }

                original(self, index, replaced, size);
                return;
            }
        }
        original(self, index, pose, size);
    }

    inline bool Install(vr::IVRServerDriverHost *host) {
        if (g.installed || !host) {
            return g.installed;
        }

        g.vtable = *reinterpret_cast<void ***>(host);
        g.original = g.vtable[kPoseUpdatedSlot];

        auto *hook = reinterpret_cast<void *>(&HookPoseUpdated);
        g.installed = PatchVtable(g.vtable, kPoseUpdatedSlot, 1, &hook);
        return g.installed;
    }

    // Must run before the driver library is unloaded, because the hook points into it.
    inline void Remove() {
        if (!g.installed) {
            return;
        }

        g.enabled.store(false);
        PatchVtable(g.vtable, kPoseUpdatedSlot, 1, &g.original);
        g.installed = false;
    }
} // namespace rebo::pose_hooks
