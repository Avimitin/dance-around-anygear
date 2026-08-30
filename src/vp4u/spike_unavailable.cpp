#include "spike.h"

namespace vp4u {

struct SpikeSource::Impl {};

std::shared_ptr<SpikeSource> SpikeSource::acquire(
    std::shared_ptr<D4xxSource>, const SpikeTrackingOptions&, std::string*) {
    return {};
}

SpikeSource::SpikeSource(
    std::shared_ptr<D4xxSource>, const SpikeTrackingOptions&) : impl_(nullptr) {}
SpikeSource::~SpikeSource() { delete impl_; }
bool SpikeSource::get_skeleton(PoseResult*) { return false; }
bool SpikeSource::wait_for_skeleton(PoseResult*, std::uint64_t*, int,
                                    std::int64_t*) {
    return false;
}
int SpikeSource::native_width() const { return 1; }
int SpikeSource::native_height() const { return 1; }
bool SpikeSource::has_color_stream() const { return false; }

} // namespace vp4u
