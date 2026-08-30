#include "vp4u/d4xx.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <librealsense2/h/rs_option.h>
#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

std::wstring widen(const char* value) {
    if (!value) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), count);
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

Json intrinsics_json(const vp4u::D4xxDepthIntrinsics& value) {
    return {
        {"width", value.width},
        {"height", value.height},
        {"principal_x", value.principal_x},
        {"principal_y", value.principal_y},
        {"focal_x", value.focal_x},
        {"focal_y", value.focal_y},
        {"distortion_model", value.distortion_model},
        {"distortion", value.distortion},
    };
}

struct DeviceRecording {
    std::ofstream depth;
    std::uint64_t last_sequence = 0;
    std::size_t frames = 0;
    Json timestamps = Json::array();
    Json metadata;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 10) {
        std::fprintf(stderr,
            "usage: anygear_d4xx_depth_record <realsense2.dll> "
            "<output-directory> <seconds> [required-devices] "
            "[infrared-index] [emitter-mode] [warmup-seconds] "
            "[depth-coordinate] [visual-preset]\n"
            "emitter-mode: unchanged, all-on, all-off, first-only, "
            "second-only, alternating\n");
        return 2;
    }
    const int seconds = std::atoi(argv[3]);
    const int required_devices = argc >= 5 ? std::atoi(argv[4]) : 2;
    const int infrared_index = argc >= 6 ? std::atoi(argv[5]) : 1;
    const std::string emitter_mode = argc >= 7 ? argv[6] : "unchanged";
    const int warmup_seconds = argc >= 8 ? std::atoi(argv[7]) : 1;
    const std::string depth_coordinate = argc >= 9 ? argv[8] : "infrared";
    const std::string visual_preset = argc >= 10 ? argv[9] : "unchanged";
    if (seconds < 1 || seconds > 300 || required_devices < 1 ||
        required_devices > 8 || infrared_index < 1 || infrared_index > 2 ||
        warmup_seconds < 0 || warmup_seconds > 30) {
        std::fprintf(stderr, "invalid recorder argument\n");
        return 2;
    }

    const std::filesystem::path output = widen(argv[2]);
    std::error_code filesystem_error;
    if (std::filesystem::exists(output, filesystem_error)) {
        if (filesystem_error || !std::filesystem::is_directory(output) ||
            std::filesystem::directory_iterator(output) !=
                std::filesystem::directory_iterator()) {
            std::fprintf(stderr,
                "output directory exists and is not empty: %s\n", argv[2]);
            return 3;
        }
    } else if (!std::filesystem::create_directories(
                   output, filesystem_error) || filesystem_error) {
        std::fprintf(stderr, "could not create output directory: %s\n", argv[2]);
        return 3;
    }

    vp4u::D4xxOptions options;
    options.runtime_path = widen(argv[1]);
    options.required_devices = required_devices;
    options.infrared_index = infrared_index;
    if (depth_coordinate == "native") {
        options.align_depth_to_infrared = false;
    } else if (depth_coordinate != "infrared") {
        std::fprintf(stderr, "unknown depth coordinate: %s\n",
                     depth_coordinate.c_str());
        return 2;
    }
    int preset_value = -1;
    if (visual_preset == "default") {
        preset_value = RS2_RS400_VISUAL_PRESET_DEFAULT;
    } else if (visual_preset == "high-accuracy") {
        preset_value = RS2_RS400_VISUAL_PRESET_HIGH_ACCURACY;
    } else if (visual_preset == "high-density") {
        preset_value = RS2_RS400_VISUAL_PRESET_HIGH_DENSITY;
    } else if (visual_preset == "medium-density") {
        preset_value = RS2_RS400_VISUAL_PRESET_MEDIUM_DENSITY;
    } else if (visual_preset != "unchanged") {
        std::fprintf(stderr, "unknown visual preset: %s\n",
                     visual_preset.c_str());
        return 2;
    }
    if (preset_value >= 0) {
        options.visual_preset_by_device.assign(
            static_cast<std::size_t>(required_devices), preset_value);
    }
    if (emitter_mode != "unchanged") {
        options.emitter_enabled_by_device.assign(
            static_cast<std::size_t>(required_devices), 1);
        options.emitter_on_off_by_device.assign(
            static_cast<std::size_t>(required_devices), 0);
        if (emitter_mode == "all-off") {
            std::fill(options.emitter_enabled_by_device.begin(),
                      options.emitter_enabled_by_device.end(), 0);
        } else if (emitter_mode == "first-only") {
            std::fill(options.emitter_enabled_by_device.begin(),
                      options.emitter_enabled_by_device.end(), 0);
            options.emitter_enabled_by_device.front() = 1;
        } else if (emitter_mode == "second-only") {
            if (required_devices < 2) {
                std::fprintf(stderr,
                    "second-only requires at least two devices\n");
                return 2;
            }
            std::fill(options.emitter_enabled_by_device.begin(),
                      options.emitter_enabled_by_device.end(), 0);
            options.emitter_enabled_by_device[1] = 1;
        } else if (emitter_mode == "alternating") {
            std::fill(options.emitter_on_off_by_device.begin(),
                      options.emitter_on_off_by_device.end(), 1);
        } else if (emitter_mode != "all-on") {
            std::fprintf(stderr, "unknown emitter mode: %s\n",
                         emitter_mode.c_str());
            return 2;
        }
    }
    std::string error;
    auto source = vp4u::D4xxSource::acquire(options, &error);
    if (!source) {
        std::fprintf(stderr, "camera open failed: %s\n", error.c_str());
        return 4;
    }
    if (!source->wait_until_ready(10000)) {
        std::fprintf(stderr, "D4xx streams did not produce aligned depth\n");
        return 5;
    }
    if (warmup_seconds > 0) {
        std::fprintf(stderr, "warming up streams for %d second(s)\n",
                     warmup_seconds);
        std::this_thread::sleep_for(std::chrono::seconds(warmup_seconds));
    }

    const int device_count = source->device_count();
    std::vector<DeviceRecording> devices(
        static_cast<std::size_t>(device_count));
    const auto serials = source->serial_numbers();
    for (int index = 0; index < device_count; ++index) {
        const std::filesystem::path path =
            output / ("device-" + std::to_string(index) + ".z16");
        devices[static_cast<std::size_t>(index)].depth.open(
            path, std::ios::binary | std::ios::trunc);
        if (!devices[static_cast<std::size_t>(index)].depth) {
            std::fprintf(stderr, "could not create %s\n",
                         path.u8string().c_str());
            return 6;
        }
        devices[static_cast<std::size_t>(index)].metadata = {
            {"index", index},
            {"serial", serials[static_cast<std::size_t>(index)]},
            {"file", path.filename().u8string()},
        };
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        bool received = false;
        for (int index = 0; index < device_count; ++index) {
            DeviceRecording& recording =
                devices[static_cast<std::size_t>(index)];
            vp4u::D4xxDepthFrame frame;
            if (!source->get_depth_frame(
                    index, recording.last_sequence, &frame)) {
                continue;
            }
            received = true;
            recording.last_sequence = frame.sequence;
            recording.depth.write(
                reinterpret_cast<const char*>(frame.depth.data()),
                static_cast<std::streamsize>(frame.depth.size() *
                                             sizeof(std::uint16_t)));
            if (!recording.depth) {
                std::fprintf(stderr, "depth write failed for device %d\n", index);
                return 7;
            }
            recording.timestamps.push_back({
                {"frame", recording.frames},
                {"sequence", frame.sequence},
                {"host_time_ns", frame.host_time_ns},
            });
            if (recording.frames == 0) {
                recording.metadata["depth_scale_m"] = frame.depth_scale_m;
                recording.metadata["intrinsics"] =
                    intrinsics_json(frame.intrinsics);
                recording.metadata["frame_bytes"] =
                    frame.depth.size() * sizeof(std::uint16_t);
            }
            ++recording.frames;
        }
        if (!received) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Json manifest = {
        {"schema", "dance-around-anygear.d4xx-depth.v1"},
        {"duration_requested_s", seconds},
        {"warmup_s", warmup_seconds},
        {"required_devices", required_devices},
        {"infrared_index", infrared_index},
        {"emitter_mode", emitter_mode},
        {"depth_coordinate", depth_coordinate},
        {"visual_preset", visual_preset},
        {"width", source->native_width()},
        {"height", source->native_height()},
        {"capture_fps", options.fps},
        {"devices", Json::array()},
    };
    for (auto& recording : devices) {
        recording.depth.close();
        recording.metadata["frame_count"] = recording.frames;
        recording.metadata["frames"] = std::move(recording.timestamps);
        manifest["devices"].push_back(std::move(recording.metadata));
    }
    const std::filesystem::path manifest_path = output / "manifest.json";
    std::ofstream manifest_stream(manifest_path, std::ios::trunc);
    manifest_stream << std::setw(2) << manifest << '\n';
    if (!manifest_stream) {
        std::fprintf(stderr, "manifest write failed\n");
        return 8;
    }
    std::printf("PASS: recorded %d device(s) to %s\n", device_count, argv[2]);
    for (int index = 0; index < device_count; ++index) {
        std::printf("      device=%d frames=%zu serial=%s\n", index,
            devices[static_cast<std::size_t>(index)].frames,
            serials[static_cast<std::size_t>(index)].c_str());
    }
    return 0;
}
