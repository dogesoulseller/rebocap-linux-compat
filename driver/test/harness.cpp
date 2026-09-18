// Stand-in for vrserver - loads driver_rebocap.so, provides stub host interfaces, runs frames.
// usage: harness <driver.so> <seconds> <universe_id> <seconds_until_headset_ready>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <openvr_driver.h>
#include <string>
#include <unistd.h>

using namespace vr;
static bool g_hmd_ready = false;
static uint64_t g_universe = 0;
static long g_pose_updates = 0;
static std::string g_chaperone_json; // served as Prop_DriverProvidedChaperoneJson_String

struct Settings : IVRSettings {
    const char *GetSettingsErrorNameFromEnum(EVRSettingsError) override {
        return "err";
    }

    void SetBool(const char *, const char *, bool, EVRSettingsError *) override {}

    void SetInt32(const char *, const char *, int32_t, EVRSettingsError *) override {}

    void SetFloat(const char *, const char *, float, EVRSettingsError *) override {}

    void SetString(const char *s, const char *k, const char *v, EVRSettingsError *) override {
        printf("[settings] %s %s = %s\n", s, k, v);
    }

    bool GetBool(const char *, const char *, EVRSettingsError *e) override {
        if (e) {
            *e = VRSettingsError_UnsetSettingHasNoDefault;
        }

        return false;
    }

    int32_t GetInt32(const char *, const char *k, EVRSettingsError *e) override {
        if (const char *port = getenv("REBO_TEST_PORT"); port && !strcmp(k, "bridge_port")) {
            if (e) {
                *e = VRSettingsError_None;
            }

            return atoi(port);
        }

        if (e) {
            *e = VRSettingsError_UnsetSettingHasNoDefault;
        }

        return 0;
    }

    float GetFloat(const char *, const char *, EVRSettingsError *e) override {
        if (e) {
            *e = VRSettingsError_UnsetSettingHasNoDefault;
        }

        return 0;
    }

    void GetString(const char *, const char *k, char *v, const uint32_t n, EVRSettingsError *e) override {
        // REBO_TEST_<KEY> (upper case) overrides a string setting, e.g. REBO_TEST_WIP_DIRECTION=waist
        std::string env = "REBO_TEST_";
        for (const char *c = k; *c; c++) {
            env += static_cast<char>(toupper(*c));
        }

        if (const char *val = getenv(env.c_str()); val && n) {
            snprintf(v, n, "%s", val);
            if (e) {
                *e = VRSettingsError_None;
            }

            return;
        }

        if (n) {
            v[0] = 0;
        }

        if (e) {
            *e = VRSettingsError_UnsetSettingHasNoDefault;
        }
    }

    void RemoveSection(const char *, EVRSettingsError *) override {}

    void RemoveKeyInSection(const char *, const char *, EVRSettingsError *) override {}
};

struct Properties : IVRProperties {
    ETrackedPropertyError ReadPropertyBatch(const PropertyContainerHandle_t c, PropertyRead_t *b, const uint32_t n) override {
        for (uint32_t i = 0; i < n; i++) {
            b[i].unRequiredBufferSize = 0;
            b[i].eError = TrackedProp_UnknownProperty;

            // Containers 50/51 are fake hand controllers of the simulated second driver.
            if ((c == 50 || c == 51) && b[i].prop == Prop_DeviceClass_Int32 && b[i].unBufferSize >= 4) {
                int32_t cls = TrackedDeviceClass_Controller;
                memcpy(b[i].pvBuffer, &cls, 4);
                b[i].unTag = k_unInt32PropertyTag;
                b[i].unRequiredBufferSize = 4;
                b[i].eError = TrackedProp_Success;
                continue;
            }

            if ((c == 50 || c == 51) && b[i].prop == Prop_ControllerRoleHint_Int32 && b[i].unBufferSize >= 4) {
                int32_t role = c == 50 ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand;
                memcpy(b[i].pvBuffer, &role, 4);
                b[i].unTag = k_unInt32PropertyTag;
                b[i].unRequiredBufferSize = 4;
                b[i].eError = TrackedProp_Success;
                continue;
            }

            if (c == 1 && g_hmd_ready && b[i].prop == Prop_DriverProvidedChaperoneJson_String &&
                !g_chaperone_json.empty()) {
                b[i].unTag = k_unStringPropertyTag;
                b[i].unRequiredBufferSize = static_cast<uint32_t>(g_chaperone_json.size() + 1);

                if (b[i].unBufferSize < b[i].unRequiredBufferSize) {
                    b[i].eError = TrackedProp_BufferTooSmall;
                    continue;
                }

                memcpy(b[i].pvBuffer, g_chaperone_json.c_str(), b[i].unRequiredBufferSize);

                b[i].eError = TrackedProp_Success;

                continue;
            }

            if (c == 1 && g_hmd_ready && b[i].prop == Prop_CurrentUniverseId_Uint64 && b[i].unBufferSize >= 8) {
                memcpy(b[i].pvBuffer, &g_universe, 8);
                b[i].unTag = k_unUint64PropertyTag;
                b[i].unRequiredBufferSize = 8;
                b[i].eError = TrackedProp_Success;
            }
        }

        return TrackedProp_Success;
    }

    ETrackedPropertyError WritePropertyBatch(PropertyContainerHandle_t, PropertyWrite_t *, uint32_t) override {
        return TrackedProp_Success;
    }

    const char *GetPropErrorNameFromEnum(ETrackedPropertyError) override {
        return "err";
    }

    PropertyContainerHandle_t TrackedDeviceToPropertyContainer(const TrackedDeviceIndex_t i) override {
        return i + 1;
    }
};

struct Host : IVRServerDriverHost {
    uint32_t next = 1;

    bool TrackedDeviceAdded(const char *serial, const ETrackedDeviceClass cls, ITrackedDeviceServerDriver *d) override {
        printf("[host] TrackedDeviceAdded %s class %d -> index %u\n", serial, cls, next);
        d->Activate(next++);
        return true;
    }

    double last49[3] = {-1, -1, -1};

    void TrackedDevicePoseUpdated(uint32_t i, const DriverPose_t &p, uint32_t size) override {
        if (size != sizeof(DriverPose_t)) {
            printf("[host] bad pose size %u\n", size);
            abort();
        }

        if (i == 49 || i == 50) {
            // the simulated second driver's hand controllers: print on change
            // The values are copies, so comparing the bytes is exact.
            // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
            if (i == 49 && memcmp(last49, p.vecPosition, sizeof last49) != 0) {
                memcpy(last49, p.vecPosition, sizeof last49);
                printf("[host] LEFT controller pose: pos %.2f %.2f %.2f  vel %.1f  world-from-driver %.2f %.2f %.2f / "
                       "qy %.2f\n",
                       p.vecPosition[0], p.vecPosition[1], p.vecPosition[2], p.vecVelocity[0],
                       p.vecWorldFromDriverTranslation[0], p.vecWorldFromDriverTranslation[1],
                       p.vecWorldFromDriverTranslation[2], p.qWorldFromDriverRotation.y);
            }
            return;
        }

        if (g_pose_updates++ % 2000 == 0) {
            printf("[host] pose idx %u pos %.2f %.2f %.2f valid %d connected %d w2d %.2f %.2f %.2f\n", i,
                   p.vecPosition[0], p.vecPosition[1], p.vecPosition[2], static_cast<int>(p.poseIsValid), static_cast<int>(p.deviceIsConnected),
                   p.vecWorldFromDriverTranslation[0], p.vecWorldFromDriverTranslation[1],
                   p.vecWorldFromDriverTranslation[2]);
        }
    }

    void VsyncEvent(double) override {}

    void VendorSpecificEvent(uint32_t, EVREventType, const VREvent_Data_t &, double) override {}

    bool IsExiting() override {
        return false;
    }

    bool PollNextEvent(VREvent_t *, uint32_t) override {
        return false;
    }

    void GetRawTrackedDevicePoses(float, TrackedDevicePose_t *poses, uint32_t n) override {
        memset(poses, 0, sizeof(TrackedDevicePose_t) * n);

        if (!n) {
            return;
        }

        auto &m = poses[0].mDeviceToAbsoluteTracking.m;
        m[0][0] = m[1][1] = m[2][2] = 1;
        m[1][3] = 1.7f;

        poses[0].bPoseIsValid = poses[0].bDeviceIsConnected = g_hmd_ready;

        // Fake left controller (property container 50 -> device index 49),
        // pointing 90 degrees to the left of the headset - its -Z axis is world -X.
        if (n > 49) {
            auto &c = poses[49].mDeviceToAbsoluteTracking.m;
            c[0][2] = 1;
            c[1][1] = 1;
            c[2][0] = -1;
            poses[49].bPoseIsValid = true;
            poses[49].bDeviceIsConnected = true;
        }
    }

    void RequestRestart(const char *, const char *, const char *, const char *) override {}

    uint32_t GetFrameTimings(Compositor_FrameTiming *, uint32_t) override {
        return 0;
    }

    void SetDisplayEyeToHead(uint32_t, const HmdMatrix34_t &, const HmdMatrix34_t &) override {}

    void SetDisplayProjectionRaw(uint32_t, const HmdRect2_t &, const HmdRect2_t &) override {}

    void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
};

struct Input : IVRDriverInput {
    std::map<VRInputComponentHandle_t, std::string> names;
    VRInputComponentHandle_t next = 100;

    EVRInputError add(const char *n, VRInputComponentHandle_t *h) {
        *h = next++;
        names[*h] = n;

        return VRInputError_None;
    }

    EVRInputError CreateBooleanComponent(PropertyContainerHandle_t, const char *n,
                                         VRInputComponentHandle_t *h) override {
        return add(n, h);
    }

    EVRInputError UpdateBooleanComponent(const VRInputComponentHandle_t h, const bool v, double) override {
        printf("[input] %s = %s\n", names[h].c_str(), v ? "true" : "false");
        return VRInputError_None;
    }

    EVRInputError CreateScalarComponent(PropertyContainerHandle_t, const char *n, VRInputComponentHandle_t *h,
                                        EVRScalarType, EVRScalarUnits) override {
        return add(n, h);
    }

    EVRInputError UpdateScalarComponent(const VRInputComponentHandle_t h, const float v, double) override {
        printf("[input] %s = %.2f\n", names[h].c_str(), v);
        return VRInputError_None;
    }

    EVRInputError CreateHapticComponent(PropertyContainerHandle_t, const char *n,
                                        VRInputComponentHandle_t *h) override {
        return add(n, h);
    }

    EVRInputError CreateSkeletonComponent(PropertyContainerHandle_t, const char *n, const char *, const char *,
                                          EVRSkeletalTrackingLevel, const VRBoneTransform_t *, uint32_t,
                                          VRInputComponentHandle_t *h) override {
        return add(n, h);
    }

    EVRInputError UpdateSkeletonComponent(VRInputComponentHandle_t, EVRSkeletalMotionRange, const VRBoneTransform_t *,
                                          uint32_t) override {
        return VRInputError_None;
    }

    EVRInputError CreatePoseComponent(PropertyContainerHandle_t, const char *n, VRInputComponentHandle_t *h) override {
        return add(n, h);
    }
    EVRInputError UpdatePoseComponent(VRInputComponentHandle_t, const HmdMatrix34_t *, double) override {
        return VRInputError_None;
    }

    EVRInputError CreateEyeTrackingComponent(PropertyContainerHandle_t, const char *n,
                                             VRInputComponentHandle_t *h) override {
        return add(n, h);
    }

    EVRInputError UpdateEyeTrackingComponent(VRInputComponentHandle_t, const VREyeTrackingData_t *, double) override {
        return VRInputError_None;
    }
};

struct DriverLog : IVRDriverLog {
    void Log(const char *m) override {
        printf("[driver] %s", m);
        fflush(stdout);
    }
};

struct Context : IVRDriverContext {
    Settings settings;
    Properties props;
    Host host;
    DriverLog log;
    Input input;

    void *GetGenericInterface(const char *v, EVRInitError *e) override {
        if (e) {
            *e = VRInitError_None;
        }

        if (!strcmp(v, IVRSettings_Version)) {
            return &settings;
        }

        if (!strcmp(v, IVRProperties_Version)) {
            return &props;
        }

        if (!strcmp(v, IVRServerDriverHost_Version)) {
            return &host;
        }

        if (!strcmp(v, IVRDriverLog_Version)) {
            return &log;
        }

        if (!strcmp(v, IVRDriverInput_Version)) {
            return &input;
        }

        // Interfaces the driver context requests at init but the driver never calls.
        static void *unused_vtable[64] = {};
        static void **unused = unused_vtable;
        return &unused;
    }

    DriverHandle_t GetDriverHandle() override {
        return 1;
    }
};

int main(int argc, char **argv) {
    if (argc < 5) {
        return 2;
    }

    const double secs = atof(argv[2]);
    const double ready_after = atof(argv[4]);
    g_universe = strtoull(argv[3], nullptr, 10);

    if (const char *f = getenv("REBO_TEST_CHAPERONE_JSON")) {
        FILE *fp = fopen(f, "rb");
        char buf[8192];
        const size_t n = fp ? fread(buf, 1, sizeof buf, fp) : 0;
        g_chaperone_json.assign(buf, n);

        if (fp) {
            fclose(fp);
        }
    }

    // REBO_TEST_GLOBAL=1 loads the driver into the global symbol scope.
    // This is the worst case for symbol conflicts with libraries loaded later.
    void *lib = dlopen(argv[1], RTLD_NOW | (getenv("REBO_TEST_GLOBAL") ? RTLD_GLOBAL : RTLD_LOCAL));
    if (!lib) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }

    const auto factory = reinterpret_cast<void *(*)(const char *, int *)>(dlsym(lib, "HmdDriverFactory"));
    int rc = 0;
    auto *provider = static_cast<IServerTrackedDeviceProvider *>(factory(IServerTrackedDeviceProvider_Version, &rc));
    if (!provider) {
        printf("factory failed %d\n", rc);
        return 1;
    }

    Context ctx;
    printf("Init -> %d\n", provider->Init(&ctx));

    // Stand-in for a Vulkan layer loaded later that parses numbers through the system libstdc++.so.
    if (getenv("REBO_TEST_LAYER")) {
        void *layer = dlopen("./liblayer.so", RTLD_NOW | RTLD_LOCAL);
        if (!layer) {
            printf("dlopen layer: %s\n", dlerror());
            return 1;
        }
        reinterpret_cast<void (*)()>(dlsym(layer, "layer_parse"))();
    }

    // Simulated second driver (vrlink).
    // It creates hand controller sticks through the same interface object type.
    // The volatile pointer prevents the compiler from bypassing the vtable.
    IVRDriverInput *volatile other = &ctx.input;
    VRInputComponentHandle_t lx = 0, ly = 0, lt = 0, rx = 0, ry = 0;

    const bool stick = getenv("REBO_TEST_STICK") != nullptr;
    if (stick) {
        other->CreateScalarComponent(50, "/input/joystick/x", &lx, VRScalarType_Absolute,
                                     VRScalarUnits_NormalizedTwoSided);
        other->CreateScalarComponent(50, "/input/joystick/y", &ly, VRScalarType_Absolute,
                                     VRScalarUnits_NormalizedTwoSided);
        other->CreateBooleanComponent(50, "/input/joystick/touch", &lt);
        other->CreateScalarComponent(51, "/input/thumbstick/x", &rx, VRScalarType_Absolute,
                                     VRScalarUnits_NormalizedTwoSided);
        other->CreateScalarComponent(51, "/input/thumbstick/y", &ry, VRScalarType_Absolute,
                                     VRScalarUnits_NormalizedTwoSided);
    }

    IVRServerDriverHost *volatile other_host = &ctx.host;
    DriverPose_t real{};
    real.qRotation = real.qWorldFromDriverRotation = real.qDriverFromHeadRotation = {.w = 1, .x = 0, .y = 0, .z = 0};
    real.vecPosition[0] = real.vecPosition[1] = real.vecPosition[2] = 9;
    real.vecVelocity[0] = 1;
    real.poseIsValid = real.deviceIsConnected = true;
    double last_real_pose = 0;
    int phase = -1;
    const auto t0 = std::chrono::steady_clock::now();

    for (;;) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t > secs) {
            break;
        }

        if (!g_hmd_ready && t > ready_after) {
            g_hmd_ready = true;
            printf("[host] headset ready, universe %llu\n", static_cast<unsigned long long>(g_universe));
        }

        if (stick && t - last_real_pose > 0.02) {
            // the second driver's controllers report at 50 Hz
            last_real_pose = t;
            other_host->TrackedDevicePoseUpdated(49, real, sizeof real);
            other_host->TrackedDevicePoseUpdated(50, real, sizeof real);
        }

        if (stick) {
            // Centered,
            // then pushed right during 2.3-2.6 s (while walking),
            // then pushed forward at 5.0 s (after walk-in-place was disabled) to check passthrough.
            if (const int p = t < 2.3 ? 0 : t < 2.6 ? 1 : t < 5.0 ? 2 : 3; p != phase) {
                phase = p;
                const float x = p == 1 ? 0.8f : 0.0f;
                const float y = p == 3 ? 0.6f : 0.0f;
                printf("[t=%.1f] real left stick -> (%.1f, %.1f); right stick y -> 0.3\n", t, x, y);
                other->UpdateScalarComponent(lx, x, 0);
                other->UpdateScalarComponent(ly, y, 0);
                other->UpdateScalarComponent(ry, 0.3f, 0);
            }
        }

        provider->RunFrame();
        usleep(1000);
    }

    provider->Cleanup();
    printf("clean exit, %ld pose updates\n", g_pose_updates);
}
