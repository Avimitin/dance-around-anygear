// Link-time Kinect placeholder for builds that contain only another backend.
#include "kinect.h"

namespace vp4u {

struct KinectSource::Impl {};

std::shared_ptr<KinectSource> KinectSource::acquire(
    const KinectTrackingOptions&) {
    return {};
}

KinectSource::KinectSource(const KinectTrackingOptions&) : impl_(nullptr) {}
KinectSource::~KinectSource() { delete impl_; }

bool KinectSource::get_frame_bgr8(std::vector<uint8_t>&, int, int) {
    return false;
}

bool KinectSource::get_skeleton(PoseResult*) { return false; }

bool KinectSource::wait_for_skeleton(PoseResult*, uint64_t*, int) {
    return false;
}

bool KinectSource::is_real_camera() const { return false; }
bool KinectSource::has_color_stream() const { return false; }
int KinectSource::native_width() const { return 0; }
int KinectSource::native_height() const { return 0; }

} // namespace vp4u
