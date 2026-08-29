// webcam.h - Media Foundation USB webcam capture.
// No external dependencies beyond system MF DLLs.
#ifndef VP4U_WEBCAM_H
#define VP4U_WEBCAM_H

#include <cstdint>
#include <memory>
#include <vector>

namespace vp4u {

// One shared capture per physical camera index (refcounted via shared_ptr).
// index < 0 selects the synthetic source used by isolated tests. A missing
// physical index never silently turns a release webcam backend synthetic.
class CameraSource {
public:
    // Acquire the shared source for physical camera `index` (0-based).
    // Never returns null; use wait_until_ready() to validate a physical source.
    static std::shared_ptr<CameraSource> acquire(int index);

    ~CameraSource();

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    // Copy the latest frame scaled into `out` (w*h*3 bytes, BGR8, top-down).
    // Returns true if a real camera frame was used, false if synthetic.
    bool get_frame_bgr8(std::vector<uint8_t>& out, int w, int h);

    // Wait for a physical source to deliver its first frame.
    bool wait_until_ready(int timeout_ms) const;

    bool is_real_camera() const;
    int native_width() const;
    int native_height() const;

private:
    explicit CameraSource(int index);
    struct Impl;
    Impl* impl_;
};

// Fill `out` (w*h*3 BGR8) with an animated synthetic test pattern.
// `tag` tints the pattern so the two virtual cameras look different.
void make_synthetic_bgr8(std::vector<uint8_t>& out, int w, int h, double time_sec, int tag);

// Bilinear-resize a BGR8 buffer (shared by the webcam and kinect sources).
void scale_bgr8_bilinear(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh);

// Number of physical video capture devices visible via Media Foundation (0 on failure).
int count_physical_cameras();

} // namespace vp4u

#endif
