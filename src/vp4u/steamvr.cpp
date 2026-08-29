#include "steamvr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <openvr_capi.h>

namespace vp4u {
namespace {

using InitInternalFn = intptr_t (__cdecl *)(EVRInitError*, EVRApplicationType);
using ShutdownInternalFn = void (__cdecl *)();
using GetGenericInterfaceFn = intptr_t (__cdecl *)(const char*, EVRInitError*);
using GetErrorDescriptionFn = const char* (__cdecl *)(EVRInitError);

template <typename T>
T proc_address(HMODULE module, const char* name) {
    FARPROC address = GetProcAddress(module, name);
    T result = nullptr;
    static_assert(sizeof(result) == sizeof(address), "function pointer size");
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

void source_log(const SteamVrTrackingOptions& options, const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (options.log) options.log(options.log_context, message);
    std::string debug = std::string("[vp4u-steamvr] ") + message + "\n";
    OutputDebugStringA(debug.c_str());
    std::fprintf(stderr, "%s", debug.c_str());
}

std::wstring join_path(const std::wstring& directory, const wchar_t* name) {
    if (directory.empty()) return name;
    const wchar_t tail = directory.back();
    return directory + ((tail == L'\\' || tail == L'/') ? L"" : L"\\") + name;
}

} // namespace

struct SteamVrSource::Impl {
    explicit Impl(const SteamVrTrackingOptions& requested) : options(requested) {
        options.capture_fps = std::clamp(options.capture_fps, 15, 120);
        options.body_hold_ms = std::clamp(options.body_hold_ms, 0, 2000);
        role_index.fill(k_unTrackedDeviceIndexInvalid);
    }

    SteamVrTrackingOptions options;
    HMODULE runtime = nullptr;
    InitInternalFn init_internal = nullptr;
    ShutdownInternalFn shutdown_internal = nullptr;
    GetGenericInterfaceFn get_interface = nullptr;
    GetErrorDescriptionFn get_error_description = nullptr;
    VR_IVRSystem_FnTable* system = nullptr;
    VR_IVRSettings_FnTable* settings = nullptr;
    bool initialized = false;
    std::array<uint32_t, kSteamVrBodyRoleCount> role_index {};
    std::string role_signature;
    SteamVrCalibration calibration;

    std::atomic<bool> stop{false};
    std::atomic<bool> active{false};
    std::thread worker;
    std::mutex skeleton_mutex;
    std::condition_variable skeleton_cv;
    PoseResult skeleton;
    uint64_t generation = 0;
    std::atomic<uint64_t> skeleton_tick{0};

    std::string property_string(uint32_t device,
                                ETrackedDeviceProperty property) const {
        if (!system) return {};
        ETrackedPropertyError error =
            ETrackedPropertyError_TrackedProp_Success;
        const uint32_t required = system->GetStringTrackedDeviceProperty(
            device, property, nullptr, 0, &error);
        // The size query may report BufferTooSmall while still returning the
        // exact required length; only the populated read must report success.
        if (required <= 1 || required > 4096) {
            return {};
        }
        std::vector<char> buffer(required, '\0');
        error = ETrackedPropertyError_TrackedProp_Success;
        system->GetStringTrackedDeviceProperty(
            device, property, buffer.data(), required, &error);
        return error == ETrackedPropertyError_TrackedProp_Success
            ? std::string(buffer.data()) : std::string();
    }

    std::string tracker_setting(const std::string& key) const {
        if (!settings || key.empty()) return {};
        char value[128] {};
        EVRSettingsError error = EVRSettingsError_VRSettingsError_None;
        char section[] = "trackers";
        std::vector<char> mutable_key(key.begin(), key.end());
        mutable_key.push_back('\0');
        settings->GetString(section, mutable_key.data(), value,
                            static_cast<uint32_t>(sizeof(value)), &error);
        return error == EVRSettingsError_VRSettingsError_None
            ? std::string(value) : std::string();
    }

    std::string role_for_tracker(uint32_t device) const {
        const std::string tracking_system = property_string(
            device, ETrackedDeviceProperty_Prop_TrackingSystemName_String);
        const std::string serial = property_string(
            device, ETrackedDeviceProperty_Prop_SerialNumber_String);
        if (!tracking_system.empty() && !serial.empty()) {
            const std::string value = tracker_setting(
                "/devices/" + tracking_system + "/" + serial);
            if (!value.empty()) return value;
        }
        const std::string registered = property_string(
            device, ETrackedDeviceProperty_Prop_RegisteredDeviceType_String);
        if (!registered.empty()) {
            const std::string value = tracker_setting("/devices/" + registered);
            if (!value.empty()) return value;
        }
        return {};
    }

    void discover_roles() {
        std::array<uint32_t, kSteamVrBodyRoleCount> next;
        next.fill(k_unTrackedDeviceIndexInvalid);
        const auto connected = [&](uint32_t device) {
            return device < k_unMaxTrackedDeviceCount &&
                   system->IsTrackedDeviceConnected(device);
        };
        if (connected(k_unTrackedDeviceIndex_Hmd)) {
            next[static_cast<size_t>(SteamVrBodyRole::Head)] =
                k_unTrackedDeviceIndex_Hmd;
        }
        const uint32_t left_hand =
            system->GetTrackedDeviceIndexForControllerRole(
                ETrackedControllerRole_TrackedControllerRole_LeftHand);
        const uint32_t right_hand =
            system->GetTrackedDeviceIndexForControllerRole(
                ETrackedControllerRole_TrackedControllerRole_RightHand);
        if (connected(left_hand)) {
            next[static_cast<size_t>(SteamVrBodyRole::LeftHand)] = left_hand;
        }
        if (connected(right_hand)) {
            next[static_cast<size_t>(SteamVrBodyRole::RightHand)] = right_hand;
        }

        for (uint32_t device = 0; device < k_unMaxTrackedDeviceCount; ++device) {
            if (!system->IsTrackedDeviceConnected(device) ||
                system->GetTrackedDeviceClass(device) !=
                    ETrackedDeviceClass_TrackedDeviceClass_GenericTracker) {
                continue;
            }
            SteamVrBodyRole role = SteamVrBodyRole::Count;
            if (steamvr_body_role_from_setting(role_for_tracker(device), &role)) {
                uint32_t& destination = next[static_cast<size_t>(role)];
                if (destination == k_unTrackedDeviceIndexInvalid) {
                    destination = device;
                }
            }
        }
        role_index = next;

        std::string signature;
        for (size_t i = 0; i < role_index.size(); ++i) {
            if (!signature.empty()) signature += ", ";
            signature += steamvr_body_role_name(static_cast<SteamVrBodyRole>(i));
            signature += "=";
            signature += role_index[i] == k_unTrackedDeviceIndexInvalid
                ? "missing" : std::to_string(role_index[i]);
        }
        if (signature != role_signature) {
            source_log(options, "device roles: %s", signature.c_str());
            role_signature = signature;
        }
    }

    bool init(std::string* error) {
        const std::wstring dll_path = join_path(options.runtime_dir,
                                                L"openvr_api.dll");
        runtime = LoadLibraryExW(
            dll_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!runtime) runtime = LoadLibraryW(dll_path.c_str());
        if (!runtime) {
            if (error) *error = "openvr_api.dll was not found in the SteamVR dependency directory";
            return false;
        }

        init_internal = proc_address<InitInternalFn>(runtime, "VR_InitInternal");
        shutdown_internal = proc_address<ShutdownInternalFn>(
            runtime, "VR_ShutdownInternal");
        get_interface = proc_address<GetGenericInterfaceFn>(
            runtime, "VR_GetGenericInterface");
        get_error_description = proc_address<GetErrorDescriptionFn>(
            runtime, "VR_GetVRInitErrorAsEnglishDescription");
        if (!init_internal || !shutdown_internal || !get_interface) {
            if (error) *error = "openvr_api.dll is missing required OpenVR exports";
            return false;
        }

        EVRInitError init_error = EVRInitError_VRInitError_None;
        init_internal(&init_error,
            EVRApplicationType_VRApplication_Background);
        if (init_error != EVRInitError_VRInitError_None) {
            const char* description = get_error_description
                ? get_error_description(init_error) : "unknown OpenVR error";
            if (error) {
                *error = std::string("OpenVR initialization failed: ") +
                         (description ? description : "unknown OpenVR error");
            }
            return false;
        }
        initialized = true;

        const std::string system_name =
            std::string("FnTable:") + IVRSystem_Version;
        init_error = EVRInitError_VRInitError_None;
        system = reinterpret_cast<VR_IVRSystem_FnTable*>(
            get_interface(system_name.c_str(), &init_error));
        if (!system || init_error != EVRInitError_VRInitError_None) {
            if (error) *error = "OpenVR IVRSystem function table is unavailable";
            return false;
        }

        const std::string settings_name =
            std::string("FnTable:") + IVRSettings_Version;
        init_error = EVRInitError_VRInitError_None;
        settings = reinterpret_cast<VR_IVRSettings_FnTable*>(
            get_interface(settings_name.c_str(), &init_error));
        if (!settings || init_error != EVRInitError_VRInitError_None) {
            settings = nullptr;
            source_log(options,
                "IVRSettings unavailable; full-body tracker roles cannot be resolved");
        }

        discover_roles();
        active = true;
        source_log(options,
            "OpenVR %lu.%lu.%lu initialized as a background pose client at %d Hz",
            k_nSteamVRVersionMajor, k_nSteamVRVersionMinor,
            k_nSteamVRVersionBuild, options.capture_fps);
        return true;
    }

    void start() {
        worker = std::thread([this] { run(); });
    }

    void run() {
        std::array<TrackedDevicePose_t, k_unMaxTrackedDeviceCount> poses {};
        const auto interval = std::chrono::microseconds(
            1000000 / std::max(1, options.capture_fps));
        auto next_tick = std::chrono::steady_clock::now();
        auto next_discovery = next_tick + std::chrono::seconds(1);
        while (!stop) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_discovery) {
                discover_roles();
                next_discovery = now + std::chrono::seconds(1);
            }

            system->GetDeviceToAbsoluteTrackingPose(
                ETrackingUniverseOrigin_TrackingUniverseStanding, 0.f,
                poses.data(), static_cast<uint32_t>(poses.size()));
            SteamVrPoseFrame frame;
            for (size_t role = 0; role < role_index.size(); ++role) {
                const uint32_t device = role_index[role];
                if (device >= poses.size()) continue;
                const TrackedDevicePose_t& tracked = poses[device];
                if (!tracked.bDeviceIsConnected || !tracked.bPoseIsValid) continue;
                SteamVrDevicePose& output = frame.device[role];
                output.tracked = true;
                output.device_index = device;
                const HmdMatrix34_t& matrix = tracked.mDeviceToAbsoluteTracking;
                output.position[0] = matrix.m[0][3];
                output.position[1] = matrix.m[1][3];
                output.position[2] = matrix.m[2][3];
                for (int row = 0; row < 3; ++row) {
                    for (int column = 0; column < 3; ++column) {
                        output.rotation[row][column] = matrix.m[row][column];
                    }
                }
            }

            PoseResult result;
            map_steamvr_pose(frame, options, &calibration, &result);
            {
                std::lock_guard<std::mutex> lock(skeleton_mutex);
                skeleton = result;
                ++generation;
            }
            skeleton_tick = GetTickCount64();
            skeleton_cv.notify_all();

            next_tick += interval;
            const auto current = std::chrono::steady_clock::now();
            if (next_tick < current) next_tick = current;
            while (!stop && std::chrono::steady_clock::now() < next_tick) {
                const auto remaining = next_tick - std::chrono::steady_clock::now();
                std::this_thread::sleep_for(
                    std::min(remaining,
                             std::chrono::steady_clock::duration(
                                 std::chrono::milliseconds(4))));
            }
        }
    }

    void shutdown() {
        stop = true;
        skeleton_cv.notify_all();
        if (worker.joinable()) worker.join();
        active = false;
        if (initialized && shutdown_internal) shutdown_internal();
        initialized = false;
        system = nullptr;
        settings = nullptr;
        if (runtime) FreeLibrary(runtime);
        runtime = nullptr;
    }
};

static std::mutex g_source_mutex;
static std::weak_ptr<SteamVrSource> g_source_instance;

std::shared_ptr<SteamVrSource> SteamVrSource::acquire(
        const SteamVrTrackingOptions& options, std::string* error) {
    std::lock_guard<std::mutex> lock(g_source_mutex);
    if (auto existing = g_source_instance.lock()) return existing;
    auto source = std::shared_ptr<SteamVrSource>(new SteamVrSource(options));
    if (!source->impl_->init(error)) return nullptr;
    source->impl_->start();
    g_source_instance = source;
    return source;
}

SteamVrSource::SteamVrSource(const SteamVrTrackingOptions& options)
    : impl_(new Impl(options)) {}

SteamVrSource::~SteamVrSource() {
    impl_->shutdown();
    delete impl_;
}

bool SteamVrSource::get_frame_bgr8(std::vector<uint8_t>& out,
                                   int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    out.assign(static_cast<size_t>(width) * height * 3, 0);
    return false;
}

bool SteamVrSource::get_skeleton(PoseResult* out) {
    if (!out || GetTickCount64() - impl_->skeleton_tick.load() > 2000) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->skeleton_mutex);
    if (impl_->generation == 0) return false;
    *out = impl_->skeleton;
    return true;
}

bool SteamVrSource::wait_for_skeleton(PoseResult* out, uint64_t* generation,
                                      int timeout_ms) {
    if (!out || !generation) return false;
    timeout_ms = std::clamp(timeout_ms, 1, 2000);
    std::unique_lock<std::mutex> lock(impl_->skeleton_mutex);
    const bool woke = impl_->skeleton_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [&] {
            return impl_->stop.load() || impl_->generation != *generation;
        });
    if (!woke || impl_->generation == *generation) return false;
    *generation = impl_->generation;
    *out = impl_->skeleton;
    return GetTickCount64() - impl_->skeleton_tick.load() <= 2000;
}

bool SteamVrSource::is_runtime_active() const {
    return impl_->active.load();
}

bool SteamVrSource::has_color_stream() const { return false; }
int SteamVrSource::native_width() const { return 1; }
int SteamVrSource::native_height() const { return 1; }

} // namespace vp4u
