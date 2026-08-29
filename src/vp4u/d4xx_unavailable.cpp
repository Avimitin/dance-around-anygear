// Link-time D4xx placeholder for builds that contain only another backend.
#include "d4xx.h"

namespace vp4u {

struct D4xxSource::Impl {};

std::shared_ptr<D4xxSource> D4xxSource::acquire(
    const D4xxOptions&, std::string*) {
    return {};
}

D4xxSource::D4xxSource(const D4xxOptions&) : impl_(nullptr) {}
D4xxSource::~D4xxSource() { delete impl_; }

bool D4xxSource::wait_until_ready(int) const { return false; }
bool D4xxSource::get_frame_bgr8(std::vector<std::uint8_t>&, int, int) {
    return false;
}
bool D4xxSource::get_pose_frame_bgr8(std::vector<std::uint8_t>&, int, int) {
    return false;
}
bool D4xxSource::apply_depth_to_pose(PoseResult*) { return false; }
int D4xxSource::device_count() const { return 0; }
int D4xxSource::active_device_count() const { return 0; }
bool D4xxSource::left_active() const { return false; }
bool D4xxSource::right_active() const { return false; }
int D4xxSource::native_width() const { return 0; }
int D4xxSource::native_height() const { return 0; }
std::vector<std::string> D4xxSource::serial_numbers() const { return {}; }

} // namespace vp4u
