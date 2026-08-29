#include "vp4u/webcam.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

std::uint64_t sample_hash(const std::vector<std::uint8_t>& frame) {
    std::uint64_t hash = 1469598103934665603ull;
    const std::size_t step = frame.size() > 4096 ? frame.size() / 4096 : 1;
    for (std::size_t index = 0; index < frame.size(); index += step) {
        hash ^= frame[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

int main(int argc, char** argv) {
    const int index = argc > 1 ? std::atoi(argv[1]) : 0;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
    if (index < 0 || seconds < 1 || seconds > 120) {
        std::fprintf(stderr, "usage: anygear_webcam_probe [camera-index] [seconds]\n");
        return 2;
    }

    const int camera_count = vp4u::count_physical_cameras();
    std::printf("Media Foundation cameras: %d\n", camera_count);
    if (index >= camera_count) {
        std::fprintf(stderr, "FAIL: camera index %d is unavailable\n", index);
        return 3;
    }

    auto camera = vp4u::CameraSource::acquire(index);
    if (!camera || !camera->wait_until_ready(5000)) {
        std::fprintf(stderr, "FAIL: camera %d produced no frame within 5 seconds\n",
                     index);
        return 4;
    }
    std::printf("Camera %d native mode: %dx%d\n", index,
                camera->native_width(), camera->native_height());

    std::vector<std::uint8_t> frame;
    std::uint64_t previous_hash = 0;
    int frames = 0;
    int changed_frames = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (camera->get_frame_bgr8(frame, 848, 480) && !frame.empty()) {
            const std::uint64_t hash = sample_hash(frame);
            if (frames > 0 && hash != previous_hash) ++changed_frames;
            previous_hash = hash;
            ++frames;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    if (frames == 0 || changed_frames == 0) {
        std::fprintf(stderr,
            "FAIL: USB camera stream did not advance (frames=%d changed=%d)\n",
            frames, changed_frames);
        return 5;
    }
    std::printf("PASS: %d sampled frames, %d changed, output 848x480 BGR8\n",
                frames, changed_frames);
    return 0;
}
