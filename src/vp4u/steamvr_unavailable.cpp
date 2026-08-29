// Link-time SteamVR placeholder for builds that contain only another backend.
#include "steamvr.h"

namespace vp4u {

struct SteamVrSource::Impl {};

std::shared_ptr<SteamVrSource> SteamVrSource::acquire(
    const SteamVrTrackingOptions&, std::string*) {
    return {};
}

SteamVrSource::SteamVrSource(const SteamVrTrackingOptions&) : impl_(nullptr) {}
SteamVrSource::~SteamVrSource() { delete impl_; }

bool SteamVrSource::get_frame_bgr8(std::vector<uint8_t>&, int, int) {
    return false;
}

bool SteamVrSource::get_skeleton(PoseResult*) { return false; }
bool SteamVrSource::wait_for_skeleton(PoseResult*, uint64_t*, int) {
    return false;
}
bool SteamVrSource::is_runtime_active() const { return false; }
bool SteamVrSource::has_color_stream() const { return false; }
int SteamVrSource::native_width() const { return 0; }
int SteamVrSource::native_height() const { return 0; }

} // namespace vp4u
