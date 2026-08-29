#include "vp4u/d4xx.h"
#include "vp4u/pose.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

std::wstring widen(const char* value) {
    if (!value) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), count);
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

void write_u16(std::ofstream& stream, std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    stream.write(bytes, sizeof bytes);
}

void write_u32(std::ofstream& stream, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    stream.write(bytes, sizeof bytes);
}

bool save_bmp(const char* path, const std::vector<std::uint8_t>& bgr,
              int width, int height) {
    const std::uint32_t row_bytes =
        (static_cast<std::uint32_t>(width) * 3u + 3u) & ~3u;
    const std::uint32_t pixel_bytes = row_bytes *
        static_cast<std::uint32_t>(height);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.put('B');
    output.put('M');
    write_u32(output, 54u + pixel_bytes);
    write_u32(output, 0);
    write_u32(output, 54);
    write_u32(output, 40);
    write_u32(output, static_cast<std::uint32_t>(width));
    write_u32(output, static_cast<std::uint32_t>(height));
    write_u16(output, 1);
    write_u16(output, 24);
    write_u32(output, 0);
    write_u32(output, pixel_bytes);
    write_u32(output, 2835);
    write_u32(output, 2835);
    write_u32(output, 0);
    write_u32(output, 0);
    const std::vector<char> padding(row_bytes -
        static_cast<std::uint32_t>(width) * 3u, 0);
    for (int row = height - 1; row >= 0; --row) {
        output.write(
            reinterpret_cast<const char*>(bgr.data()) +
                static_cast<std::size_t>(row) * width * 3,
            static_cast<std::streamsize>(width) * 3);
        output.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    }
    return output.good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4 && argc != 6 && argc != 8) {
        std::fprintf(stderr,
            "usage: anygear_d4xx_probe <realsense2.dll> <output.bmp> "
            "[seconds [mediapipe-dir model.task "
            "[primary-device infrared-index]]]\n");
        return 2;
    }
    const int seconds = argc >= 4 ? std::atoi(argv[3]) : 5;
    if (seconds < 1 || seconds > 120) return 2;

    vp4u::D4xxOptions options;
    options.runtime_path = widen(argv[1]);
    options.required_devices = 2;
    options.primary_device = argc == 8 ? std::atoi(argv[6]) : 0;
    options.infrared_index = argc == 8 ? std::atoi(argv[7]) : 1;
    if (options.primary_device < 0 || options.primary_device > 31 ||
        options.infrared_index < 1 || options.infrared_index > 2) {
        return 2;
    }
    std::string error;
    auto source = vp4u::D4xxSource::acquire(options, &error);
    if (!source) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 3;
    }
    if (!source->wait_until_ready(8000)) {
        std::fprintf(stderr, "FAIL: D4xx streams produced no IR/depth frame\n");
        return 4;
    }
    std::printf("READY: devices=%d active=%d frame=%dx%d\n",
        source->device_count(), source->active_device_count(),
        source->native_width(), source->native_height());
    for (const auto& serial : source->serial_numbers()) {
        std::printf("       serial=%s\n", serial.c_str());
    }

    vp4u::PoseEngine pose;
    bool infer = false;
    if (argc == 6 || argc == 8) {
        vp4u::PoseCalib calibration;
        if (!pose.init(widen(argv[4]), argv[5], calibration, &error)) {
            std::fprintf(stderr, "FAIL: %s\n", error.c_str());
            return 5;
        }
        infer = true;
    }

    const int width = 848;
    const int height = 480;
    std::vector<std::uint8_t> frame;
    int frames = 0;
    int tracked = 0;
    double confidence_sum = 0.0;
    bool last_tracked = false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (source->get_pose_frame_bgr8(frame, width, height)) {
            ++frames;
            if (infer) {
                vp4u::PoseResult result;
                const bool valid = pose.infer(frame.data(), width, height,
                                              &result) && result.valid;
                if (valid) {
                    source->apply_depth_to_pose(&result);
                    ++tracked;
                    confidence_sum += result.confidence;
                }
                if (valid != last_tracked) {
                    std::printf("POSE: %s confidence=%.3f\n",
                        valid ? "TRACKED" : "NOT TRACKED", result.confidence);
                    last_tracked = valid;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    if (frame.empty() || !save_bmp(argv[2], frame, width, height)) {
        std::fprintf(stderr, "FAIL: could not save IR preview BMP\n");
        return 6;
    }
    std::printf(
        "PASS: RGB disabled; primary=%d IR=%d captured=%d tracked=%d "
        "rate=%.1f%% avg-confidence=%.3f output=%s\n",
        options.primary_device, options.infrared_index, frames, tracked,
        frames > 0 ? 100.0 * tracked / frames : 0.0,
        tracked > 0 ? confidence_sum / tracked : 0.0, argv[2]);
    return 0;
}
