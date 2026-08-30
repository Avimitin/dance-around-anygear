// SPiKE depth-pose worker exposed as a hardware skeleton source.
#ifndef VP4U_SPIKE_H
#define VP4U_SPIKE_H

#include "d4xx.h"
#include "pose.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vp4u {

struct SpikeTrackingOptions {
    std::wstring worker_executable;
    std::vector<std::wstring> worker_arguments;
    std::uint32_t worker_startup_timeout_ms = 45000;
    bool face_to_face = true;
    bool auto_center_z = true;
    float stage_hip_z = 2.30f;
    float mirror_center_x = 0.55f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    float pitch_degrees = 0.0f;
    float yaw_degrees = 0.0f;
    float roll_degrees = 0.0f;
    float smoothing = 1.0f;
    D4xxLogCallback log = nullptr;
    void* log_context = nullptr;
};

class SpikeSource {
public:
    static std::shared_ptr<SpikeSource> acquire(
        std::shared_ptr<D4xxSource> cameras,
        const SpikeTrackingOptions& options,
        std::string* error = nullptr);

    ~SpikeSource();
    SpikeSource(const SpikeSource&) = delete;
    SpikeSource& operator=(const SpikeSource&) = delete;

    bool get_skeleton(PoseResult* output);
    bool wait_for_skeleton(PoseResult* output, std::uint64_t* generation,
                           int timeout_ms,
                           std::int64_t* source_time_ns = nullptr);
    int native_width() const;
    int native_height() const;
    bool has_color_stream() const;

private:
    SpikeSource(std::shared_ptr<D4xxSource> cameras,
                const SpikeTrackingOptions& options);
    struct Impl;
    Impl* impl_;
};

} // namespace vp4u

#endif // VP4U_SPIKE_H
