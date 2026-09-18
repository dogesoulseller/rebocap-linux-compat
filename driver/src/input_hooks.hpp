// Walk in place injection into the real hand controller's thumbstick.
//
// Most games read locomotion only from the left and right hand controllers and ignore a separate treadmill device.
// The vendor's Windows driver uses inline detours on IVRDriverInput.
// This driver hooks into the same four functions by patching the vtable of vrserver's IVRDriverInput implementation.
// All drivers that use the same interface version share that vtable.
// The hooks modify updates to the selected hand's stick while walk-in-place output is active.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <openvr_driver.h>

#include "vtable.hpp"

namespace rebo::hooks {
    // Slot order follows the declaration order in IVRDriverInput (no virtual destructor).
    enum Slot { kCreateBoolean = 0, kUpdateBoolean = 1, kCreateScalar = 2, kUpdateScalar = 3, kSlotCount = 4 };

    using CreateBooleanFn = vr::EVRInputError (*)(vr::IVRDriverInput *, vr::PropertyContainerHandle_t, const char *,
                                                  vr::VRInputComponentHandle_t *);
    using UpdateBooleanFn = vr::EVRInputError (*)(vr::IVRDriverInput *, vr::VRInputComponentHandle_t, bool, double);
    using CreateScalarFn = vr::EVRInputError (*)(vr::IVRDriverInput *, vr::PropertyContainerHandle_t, const char *,
                                                 vr::VRInputComponentHandle_t *, vr::EVRScalarType, vr::EVRScalarUnits);
    using UpdateScalarFn = vr::EVRInputError (*)(vr::IVRDriverInput *, vr::VRInputComponentHandle_t, float, double);

    struct Stick {
        std::atomic<vr::IVRDriverInput *> owner{nullptr}; // the creating driver's interface object
        std::atomic<vr::PropertyContainerHandle_t> container{0}; // the controller device the stick belongs to
        std::atomic<vr::VRInputComponentHandle_t> x{0}, y{0}, touch{0};
        std::atomic<float> real_x{0}, real_y{0};
        std::atomic<bool> real_touch{false};

        bool complete() const {
            return owner.load() != nullptr && x.load() != 0 && y.load() != 0;
        }
    };

    struct Pending {
        vr::IVRDriverInput *owner;
        vr::PropertyContainerHandle_t container;
        vr::VRInputComponentHandle_t handle;
        char axis; // 'x', 'y' or 't'
    };

    struct State {
        void **vtable = nullptr;
        void *original[kSlotCount] = {};
        bool installed = false;
        Stick sticks[2]; // 0 = left hand, 1 = right hand
        std::atomic<int> overriding{-1}; // index of the stick whose real updates are withheld, or -1
        float written_x = 0, written_y = 0; // last injected values (frame thread only)
        std::mutex pending_mutex;
        std::vector<Pending> pending; // stick components whose hand is not known yet
        std::atomic<int> foreign_components{0}; // components created by other drivers and seen by the hooks
    };

    inline State g;

    inline bool EndsWith(const char *s, const char *suffix) {
        const size_t n = std::strlen(s);
        const size_t m = std::strlen(suffix);
        return n >= m && std::strcmp(s + n - m, suffix) == 0;
    }

    inline void Remember(vr::IVRDriverInput *self, const vr::PropertyContainerHandle_t c, const char *name,
                         const vr::VRInputComponentHandle_t h, const bool boolean) {
        if (!name) {
            return;
        }

        if (self != vr::VRDriverInput()) {
            ++g.foreign_components;
        }

        char axis = 0;
        if (boolean) {
            if (EndsWith(name, "/input/joystick/touch") || EndsWith(name, "/input/thumbstick/touch")) {
                axis = 't';
            }
        } else {
            if (EndsWith(name, "/input/joystick/x") || EndsWith(name, "/input/thumbstick/x")) {
                axis = 'x';
            }

            if (EndsWith(name, "/input/joystick/y") || EndsWith(name, "/input/thumbstick/y")) {
                axis = 'y';
            }
        }

        if (!axis) {
            return;
        }

        std::lock_guard lock(g.pending_mutex);

        if (g.pending.size() >= 64) {
            g.pending.erase(g.pending.begin()); // drop the oldest, some devices never get a role
        }

        g.pending.push_back({.owner = self, .container = c, .handle = h, .axis = axis});
    }

    inline vr::EVRInputError HookCreateBoolean(vr::IVRDriverInput *self, const vr::PropertyContainerHandle_t c,
                                               const char *name, vr::VRInputComponentHandle_t *h) {
        const auto r = reinterpret_cast<CreateBooleanFn>(g.original[kCreateBoolean])(self, c, name, h);
        if (r == vr::VRInputError_None && h) {
            Remember(self, c, name, *h, true);
        }

        return r;
    }

    inline vr::EVRInputError HookCreateScalar(vr::IVRDriverInput *self, const vr::PropertyContainerHandle_t c,
                                              const char *name, vr::VRInputComponentHandle_t *h, const vr::EVRScalarType type,
                                              const vr::EVRScalarUnits units) {
        const auto r = reinterpret_cast<CreateScalarFn>(g.original[kCreateScalar])(self, c, name, h, type, units);
        if (r == vr::VRInputError_None && h) {
            Remember(self, c, name, *h, false);
        }

        return r;
    }

    inline vr::EVRInputError HookUpdateScalar(vr::IVRDriverInput *self, const vr::VRInputComponentHandle_t h, const float v,
                                              const double t) {
        for (int i = 0; i < 2; i++) {
            Stick &s = g.sticks[i];

            const bool is_x = h == s.x.load();
            const bool is_y = h == s.y.load();
            if (!h || (!is_x && !is_y)) {
                continue;
            }

            (is_x ? s.real_x : s.real_y).store(v);

            if (g.overriding.load() == i) {
                return vr::VRInputError_None; // not forwarded, used only for steering
            }
        }

        return reinterpret_cast<UpdateScalarFn>(g.original[kUpdateScalar])(self, h, v, t);
    }

    inline vr::EVRInputError HookUpdateBoolean(vr::IVRDriverInput *self, const vr::VRInputComponentHandle_t h, const bool v,
                                               const double t) {
        for (int i = 0; i < 2; i++) {
            Stick &s = g.sticks[i];
            if (!h || h != s.touch.load()) {
                continue;
            }

            s.real_touch.store(v);
            if (g.overriding.load() == i) {
                return vr::VRInputError_None;
            }
        }

        return reinterpret_cast<UpdateBooleanFn>(g.original[kUpdateBoolean])(self, h, v, t);
    }

    inline bool WriteSlots(void *const *values) {
        return PatchVtable(g.vtable, 0, kSlotCount, values);
    }

    inline bool Install(vr::IVRDriverInput *input) {
        if (g.installed || !input) {
            return g.installed;
        }

        g.vtable = *reinterpret_cast<void ***>(input);

        for (int i = 0; i < kSlotCount; i++) {
            g.original[i] = g.vtable[i];
        }

        void *hooks[kSlotCount] = {};
        hooks[kCreateBoolean] = reinterpret_cast<void *>(&HookCreateBoolean);
        hooks[kUpdateBoolean] = reinterpret_cast<void *>(&HookUpdateBoolean);
        hooks[kCreateScalar] = reinterpret_cast<void *>(&HookCreateScalar);
        hooks[kUpdateScalar] = reinterpret_cast<void *>(&HookUpdateScalar);
        g.installed = WriteSlots(hooks);

        return g.installed;
    }

    // Must run before the driver library is unloaded, because the hooks point into it.
    inline void Remove() {
        if (!g.installed) {
            return;
        }

        g.overriding.store(-1);
        WriteSlots(g.original);
        g.installed = false;
    }

    // Assigns remembered stick components to a hand once the owning device's role is known.
    // Runs on the driver's frame thread. Returns true if a stick became complete.
    inline bool ResolvePending() {
        std::vector<Pending> todo;
        {
            std::lock_guard lock(g.pending_mutex);
            todo.swap(g.pending);
        }

        if (todo.empty()) {
            return false;
        }

        bool completed = false;
        std::vector<Pending> keep;
        for (const Pending &p: todo) {
            vr::ETrackedPropertyError err = vr::TrackedProp_Success;
            const int32_t role = vr::VRProperties()->GetInt32Property(p.container, vr::Prop_ControllerRoleHint_Int32, &err);
            if (err != vr::TrackedProp_Success) {
                keep.push_back(p);
                continue;
            } // role not set yet

            if (role != vr::TrackedControllerRole_LeftHand && role != vr::TrackedControllerRole_RightHand) {
                continue;
            }

            Stick &s = g.sticks[role == vr::TrackedControllerRole_LeftHand ? 0 : 1];
            const bool was = s.complete();
            s.owner.store(p.owner);
            s.container.store(p.container);
            (p.axis == 'x' ? s.x : p.axis == 'y' ? s.y : s.touch).store(p.handle);
            completed = completed || (!was && s.complete());
        }

        if (!keep.empty()) {
            std::lock_guard lock(g.pending_mutex);
            g.pending.insert(g.pending.end(), keep.begin(), keep.end());
        }

        return completed;
    }

    // speed <= 0 releases the stick. The real values are written back and updates pass through again.
    // (dir_x, dir_y) is the unit stick direction used while the real stick is centered.
    inline void Drive(const int hand, const float speed, const float dir_x = 0, const float dir_y = 1) {
        const int current = g.overriding.load();
        if (speed <= 0 || hand < 0 || hand > 1 || !g.sticks[hand].complete()) {
            if (current < 0) {
                return;
            }

            const Stick &s = g.sticks[current];
            g.overriding.store(-1);
            reinterpret_cast<UpdateScalarFn>(g.original[kUpdateScalar])(s.owner, s.x, s.real_x, 0);
            reinterpret_cast<UpdateScalarFn>(g.original[kUpdateScalar])(s.owner, s.y, s.real_y, 0);

            if (s.touch.load()) {
                reinterpret_cast<UpdateBooleanFn>(g.original[kUpdateBoolean])(s.owner, s.touch, s.real_touch, 0);
            }

            return;
        }

        if (current >= 0 && current != hand) {
            Drive(current, 0); // hand changed, release the other stick
        }

        const Stick &s = g.sticks[hand];

        // Deflected real stick sets the direction.
        // If centered, the direction is (dir_x, dir_y).
        const float rx = s.real_x;
        const float ry = s.real_y;
        const float len = std::hypot(rx, ry);
        float x = dir_x * speed, y = dir_y * speed;

        if (len > 0.15f) {
            x = rx / len * speed;
            y = ry / len * speed;
        }

        if (g.overriding.load() == hand && x == g.written_x && y == g.written_y) {
            return;
        }

        g.overriding.store(hand);
        g.written_x = x;
        g.written_y = y;

        reinterpret_cast<UpdateScalarFn>(g.original[kUpdateScalar])(s.owner, s.x, x, 0);
        reinterpret_cast<UpdateScalarFn>(g.original[kUpdateScalar])(s.owner, s.y, y, 0);
        if (s.touch.load()) {
            reinterpret_cast<UpdateBooleanFn>(g.original[kUpdateBoolean])(s.owner, s.touch, true, 0);
        }
    }
} // namespace rebo::hooks
