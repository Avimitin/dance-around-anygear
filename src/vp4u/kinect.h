// kinect.h - Kinect for Windows v1 sensor as camera + skeleton source.
// Dynamically loads Kinect10.dll at runtime (no import-lib dependency).
#ifndef VP4U_KINECT_H
#define VP4U_KINECT_H

#include <cstdint>
#include <memory>
#include <vector>

#include "pose.h"   // PoseResult, MpLandmark

namespace vp4u {

// Runtime controls for Kinect's native skeleton smoother.  Kinect v1 already
// produces a hardware-tracked 30 Hz skeleton, so the compatibility layer uses
// a low-latency profile instead of stacking a heavy software filter on top.
struct KinectTrackingOptions {
    float smoothing = 0.35f;
    float correction = 0.65f;
    float prediction = 0.00f;
    float jitter_radius = 0.05f;
    float max_deviation_radius = 0.04f;
    // Keep the selected Kinect tracking ID through momentary NUI dropouts so
    // another person/ghost skeleton cannot replace the performer for one
    // frame. The compatibility layer separately holds the last complete body for
    // body_hold_ms before reporting a genuine loss.
    int skeleton_lock_ms = 400;
    int body_hold_ms = 500;
    // Per-joint residual speed after parent/body translation is removed.
    // This is deliberately generous enough for dancing; it rejects only
    // single-frame Kinect endpoint explosions.
    float max_joint_speed_mps = 8.0f;
    float bone_length_tolerance = 0.35f;
    bool use_color = false;
    // The game backend leaves this disabled. Research capture can opt in to
    // the native 320x240 depth stream without enabling RGB processing.
    bool capture_depth = false;
    int color_fps = 15;
};

struct KinectDepthFrame {
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    std::int64_t host_time_ns = 0;
    // Kinect frame clock in milliseconds since sensor initialization.
    std::int64_t sensor_time_ms = 0;
    int width = 0;
    int height = 0;
    // Millimetres with the Kinect player-index bits removed.
    std::vector<std::uint16_t> depth_mm;
    // NUI's hardware player segmentation (0 background, 1..6 tracked slot).
    std::vector<std::uint8_t> player_index;
};

// Singleton around the single Kinect v1 sensor (NuiInitialize is process-global).
// Mirrors the CameraSource interface so the shim can swap backends freely.
// NUI skeleton tracking provides the 20-joint body directly; the Kinect
// backend does not load MediaPipe or run image-based pose inference.
class KinectSource {
public:
    // Open sensor 0 and start streaming. Returns null on any failure
    // (no Kinect10.dll, no device, init error). An explicitly selected Kinect
    // backend never falls through to a different physical device.
    static std::shared_ptr<KinectSource> acquire(
        const KinectTrackingOptions& options = KinectTrackingOptions());

    ~KinectSource();

    KinectSource(const KinectSource&) = delete;
    KinectSource& operator=(const KinectSource&) = delete;

    // Same semantics as CameraSource::get_frame_bgr8: copy of the latest color
    // frame scaled into `out` (w*h*3 BGR8 top-down). Returns true if a real
    // Kinect frame was used, false if the synthetic fallback pattern.
    bool get_frame_bgr8(std::vector<uint8_t>& out, int w, int h);

    // Latest skeleton mapped into a PoseResult (NUI 20 joints -> MediaPipe 33
    // landmarks; out->valid=false when nobody is tracked). Returns false when
    // no skeleton data has arrived yet or the stream stalled.
    bool get_skeleton(PoseResult* out);

    // Wait until NUI publishes a skeleton frame newer than `generation`.
    // A frame with out->valid=false still counts: it tells the callback consumer
    // that tracking was lost without repeatedly submitting stale poses.
    bool wait_for_skeleton(PoseResult* out, uint64_t* generation, int timeout_ms,
                           std::int64_t* host_time_ns = nullptr,
                           std::uint32_t* user_index = nullptr,
                           PoseResult* prefilter_out = nullptr,
                           std::int64_t* sensor_time_ms = nullptr);

    // Wait for a native depth frame newer than `generation`. This path exists
    // for offline sensor calibration and is inactive in the game backend.
    bool wait_for_depth(KinectDepthFrame* out, std::uint64_t* generation,
                        int timeout_ms);

    bool is_real_camera() const;   // color frames are arriving
    bool has_color_stream() const; // false for skeleton-only low-load mode
    bool has_depth_stream() const;
    int native_width() const;
    int native_height() const;

private:
    explicit KinectSource(const KinectTrackingOptions& options);
    struct Impl;
    Impl* impl_;
};

} // namespace vp4u

#endif // VP4U_KINECT_H
