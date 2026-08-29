// Intel RealSense D4xx depth/infrared capture behind a small runtime-loaded ABI.
#ifndef VP4U_D4XX_H
#define VP4U_D4XX_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pose.h"

namespace vp4u {

using D4xxLogCallback = void (*)(void* context, const char* message);

struct D4xxOptions {
    std::wstring runtime_path;
    int width = 848;
    int height = 480;
    int fps = 30;
    int primary_device = 0;
    std::string primary_serial;
    int required_devices = 2;
    int infrared_index = 1;
    float depth_scale_m = 0.001f;
    float mirror_center_x = 0.55f;
    bool face_to_face = true;
    bool auto_center_z = true;
    float stage_hip_z = 2.30f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    float pitch_degrees = 0.0f;
    float yaw_degrees = 0.0f;
    float roll_degrees = 0.0f;
    float smoothing = 0.45f;
    D4xxLogCallback log = nullptr;
    void* log_context = nullptr;
};

class D4xxSource {
public:
    static std::shared_ptr<D4xxSource> acquire(
        const D4xxOptions& options, std::string* error = nullptr);

    ~D4xxSource();
    D4xxSource(const D4xxSource&) = delete;
    D4xxSource& operator=(const D4xxSource&) = delete;

    bool wait_until_ready(int timeout_ms) const;
    bool get_frame_bgr8(std::vector<std::uint8_t>& out, int width, int height);

    // Capture the infrared pixels and their aligned depth map as one pose
    // packet. apply_depth_to_pose() consumes the matching depth snapshot after
    // inference, avoiding a one-frame skew during fast limb motion.
    bool get_pose_frame_bgr8(
        std::vector<std::uint8_t>& out, int width, int height);

    // Replace MediaPipe's monocular coordinates with metric coordinates from
    // the depth frame at the detected IR pixels. Missing depth points retain a
    // hip-relative MediaPipe estimate so one occluded joint does not drop the
    // whole body.
    bool apply_depth_to_pose(PoseResult* pose);

    int device_count() const;
    int active_device_count() const;
    bool left_active() const;
    bool right_active() const;
    int native_width() const;
    int native_height() const;
    std::vector<std::string> serial_numbers() const;

private:
    explicit D4xxSource(const D4xxOptions& options);
    struct Impl;
    Impl* impl_;
};

} // namespace vp4u

#endif // VP4U_D4XX_H
