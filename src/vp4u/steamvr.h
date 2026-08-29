// SteamVR tracked-device poses exposed as a VP4U skeleton source.
#ifndef VP4U_STEAMVR_H
#define VP4U_STEAMVR_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pose.h"

namespace vp4u {

enum class SteamVrBodyRole : size_t {
    Head = 0,
    LeftHand,
    RightHand,
    Waist,
    Chest,
    LeftShoulder,
    RightShoulder,
    LeftElbow,
    RightElbow,
    LeftKnee,
    RightKnee,
    LeftFoot,
    RightFoot,
    Count
};

constexpr size_t kSteamVrBodyRoleCount =
    static_cast<size_t>(SteamVrBodyRole::Count);

struct SteamVrDevicePose {
    bool tracked = false;
    uint32_t device_index = UINT32_MAX;
    float position[3] {};
    // Row-major 3x3 rotation from device-local axes to standing space.
    float rotation[3][3] {
        {1.f, 0.f, 0.f},
        {0.f, 1.f, 0.f},
        {0.f, 0.f, 1.f}
    };
};

struct SteamVrPoseFrame {
    std::array<SteamVrDevicePose, kSteamVrBodyRoleCount> device {};
};

using SteamVrLogCallback = void (*)(void* context, const char* message);

struct SteamVrTrackingOptions {
    std::wstring runtime_dir;
    int capture_fps = 60;
    int body_hold_ms = 250;
    float stage_hip_x = 0.55f;
    float stage_hip_y = 0.0f;
    float stage_hip_z = 2.30f;
    bool auto_center = true;
    bool face_to_face = true;
    // Six-point tracking is the reliable default: HMD, both controllers,
    // waist and both feet. These switches permit an explicit three-point
    // evaluation mode without presenting inferred legs as tracked hardware.
    bool require_waist = true;
    bool require_feet = true;
    SteamVrLogCallback log = nullptr;
    void* log_context = nullptr;
};

struct SteamVrCalibration {
    bool ready = false;
    float origin[3] {};
    float right[3] {1.f, 0.f, 0.f};
    float forward[3] {0.f, 0.f, -1.f};
};

const char* steamvr_body_role_name(SteamVrBodyRole role);
bool steamvr_body_role_from_setting(const std::string& setting,
                                    SteamVrBodyRole* role);

// Pure pose conversion used by the runtime source and deterministic tests.
// It maps OpenVR standing-space meters to the same MediaPipe-indexed stage
// representation consumed by the shared VP4U body mapper.
bool map_steamvr_pose(const SteamVrPoseFrame& frame,
                      const SteamVrTrackingOptions& options,
                      SteamVrCalibration* calibration,
                      PoseResult* output);

class SteamVrSource {
public:
    static std::shared_ptr<SteamVrSource> acquire(
        const SteamVrTrackingOptions& options, std::string* error = nullptr);

    ~SteamVrSource();
    SteamVrSource(const SteamVrSource&) = delete;
    SteamVrSource& operator=(const SteamVrSource&) = delete;

    bool get_frame_bgr8(std::vector<uint8_t>& out, int width, int height);
    bool get_skeleton(PoseResult* out);
    bool wait_for_skeleton(PoseResult* out, uint64_t* generation,
                           int timeout_ms);
    bool is_runtime_active() const;
    bool has_color_stream() const;
    int native_width() const;
    int native_height() const;

private:
    explicit SteamVrSource(const SteamVrTrackingOptions& options);
    struct Impl;
    Impl* impl_;
};

} // namespace vp4u

#endif // VP4U_STEAMVR_H
