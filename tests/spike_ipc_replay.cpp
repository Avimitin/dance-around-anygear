#include "d4xx/spike_ipc.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

struct Recording {
    std::filesystem::path path;
    std::ifstream stream;
    std::string serial;
    float depth_scale_m = 0.001f;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float principal_x = 0.0f;
    float principal_y = 0.0f;
    float focal_x = 0.0f;
    float focal_y = 0.0f;
    std::int32_t distortion_model = 0;
    float distortion[5]{};
    std::vector<std::uint64_t> sequence;
    std::vector<std::int64_t> host_time_ns;
    std::vector<std::uint16_t> depth;
};

std::vector<Recording> load_recordings(const std::filesystem::path& root) {
    std::ifstream manifest_stream(root / "manifest.json");
    if (!manifest_stream) throw std::runtime_error("manifest.json is absent");
    Json manifest;
    manifest_stream >> manifest;
    if (manifest.value("schema", "") !=
        "dance-around-anygear.d4xx-depth.v1") {
        throw std::runtime_error("unsupported depth recording schema");
    }
    std::vector<Recording> result;
    for (const Json& device : manifest.at("devices")) {
        Recording recording;
        recording.path = root / device.at("file").get<std::string>();
        recording.serial = device.at("serial").get<std::string>();
        recording.depth_scale_m = device.at("depth_scale_m").get<float>();
        const Json& intrinsics = device.at("intrinsics");
        recording.width = intrinsics.at("width").get<std::uint32_t>();
        recording.height = intrinsics.at("height").get<std::uint32_t>();
        recording.principal_x = intrinsics.at("principal_x").get<float>();
        recording.principal_y = intrinsics.at("principal_y").get<float>();
        recording.focal_x = intrinsics.at("focal_x").get<float>();
        recording.focal_y = intrinsics.at("focal_y").get<float>();
        recording.distortion_model =
            intrinsics.at("distortion_model").get<std::int32_t>();
        const Json& distortion = intrinsics.at("distortion");
        for (std::size_t index = 0;
             index < std::min<std::size_t>(5, distortion.size()); ++index) {
            recording.distortion[index] = distortion[index].get<float>();
        }
        for (const Json& frame : device.at("frames")) {
            recording.sequence.push_back(
                frame.at("sequence").get<std::uint64_t>());
            recording.host_time_ns.push_back(
                frame.at("host_time_ns").get<std::int64_t>());
        }
        recording.depth.resize(
            static_cast<std::size_t>(recording.width) * recording.height);
        recording.stream.open(recording.path, std::ios::binary);
        if (!recording.stream) {
            throw std::runtime_error(
                "could not open " + recording.path.string());
        }
        result.push_back(std::move(recording));
    }
    if (result.size() < anygear::spike::kCameraCount) {
        throw std::runtime_error("replay requires two camera recordings");
    }
    return result;
}

void read_frame(Recording& recording, std::size_t frame_index) {
    const std::streamoff frame_bytes = static_cast<std::streamoff>(
        recording.depth.size() * sizeof(std::uint16_t));
    recording.stream.seekg(
        static_cast<std::streamoff>(frame_index) * frame_bytes,
        std::ios::beg);
    recording.stream.read(
        reinterpret_cast<char*>(recording.depth.data()), frame_bytes);
    if (recording.stream.gcount() != frame_bytes) {
        throw std::runtime_error("depth recording is truncated");
    }
}

} // namespace

int main(int argument_count, char** arguments) {
    if (argument_count != 6 && argument_count != 7) {
        std::cerr << "usage: anygear_spike_ipc_replay <worker-or-python.exe> "
                     "<model> <runtime config> "
                     "<capture root> <worker log> [frames]\n";
        return 2;
    }
    try {
        const std::filesystem::path worker = arguments[1];
        const std::filesystem::path model = arguments[2];
        const std::filesystem::path runtime_config = arguments[3];
        const std::filesystem::path capture_root = arguments[4];
        const std::filesystem::path worker_log = arguments[5];
        const std::size_t requested_frames = argument_count == 7
            ? static_cast<std::size_t>(std::stoul(arguments[6]))
            : 12;
        auto recordings = load_recordings(capture_root);
        const std::size_t available_frames = std::min(
            recordings[0].sequence.size(), recordings[1].sequence.size());
        const std::size_t frame_count = std::min(
            requested_frames, available_frames);

        anygear::spike::LaunchOptions options;
        options.executable = worker.wstring();
        std::wstring executable_name = worker.filename().wstring();
        std::transform(
            executable_name.begin(), executable_name.end(),
            executable_name.begin(),
            [](wchar_t value) { return std::towlower(value); });
        if (executable_name.rfind(L"python", 0) == 0) {
            options.arguments = {L"-m", L"anygear_spike.worker"};
        }
        options.arguments.insert(options.arguments.end(), {
            L"--model", model.wstring(),
            L"--runtime-config", runtime_config.wstring(),
            L"--log", worker_log.wstring(),
        });
        options.startup_timeout_ms = 60000;
        options.shutdown_timeout_ms = 3000;
        std::string error;
        auto host = anygear::spike::IpcHost::launch(options, &error);
        if (!host) throw std::runtime_error(error);

        std::uint64_t pose_generation = 0;
        std::size_t valid_poses = 0;
        std::vector<std::uint32_t> worker_times;
        for (std::size_t frame_index = 0;
             frame_index < frame_count; ++frame_index) {
            if (frame_index != 0) Sleep(35);
            for (std::size_t camera = 0;
                 camera < anygear::spike::kCameraCount; ++camera) {
                Recording& recording = recordings[camera];
                read_frame(recording, frame_index);
                anygear::spike::DepthInput input;
                input.frame_sequence = recording.sequence[frame_index];
                input.host_time_ns = recording.host_time_ns[frame_index];
                input.depth_scale_m = recording.depth_scale_m;
                input.width = recording.width;
                input.height = recording.height;
                input.principal_x = recording.principal_x;
                input.principal_y = recording.principal_y;
                input.focal_x = recording.focal_x;
                input.focal_y = recording.focal_y;
                input.distortion_model = recording.distortion_model;
                std::copy(std::begin(recording.distortion),
                          std::end(recording.distortion),
                          std::begin(input.distortion));
                input.device_index = camera;
                input.serial = recording.serial;
                input.depth = recording.depth.data();
                input.depth_samples = recording.depth.size();
                if (!host->publish_depth(camera, input, &error)) {
                    throw std::runtime_error(error);
                }
            }
            anygear::spike::PoseFrame pose{};
            if (!host->wait_for_pose(
                    pose_generation, 5000, &pose)) {
                throw std::runtime_error(
                    "pose timeout, state=" +
                    std::to_string(static_cast<int>(host->worker_state())) +
                    ", error=" + host->worker_error());
            }
            pose_generation = pose.generation;
            worker_times.push_back(pose.worker_time_us);
            if ((pose.flags & anygear::spike::pose_fused_valid) != 0) {
                ++valid_poses;
            }
            for (const auto& joint : pose.fused_joints) {
                for (const float coordinate : joint) {
                    if (!std::isfinite(coordinate)) {
                        throw std::runtime_error(
                            "worker returned a non-finite joint");
                    }
                }
            }
        }
        std::sort(worker_times.begin(), worker_times.end());
        const auto percentile_ms = [&](double percentile) {
            if (worker_times.empty()) return 0.0;
            const std::size_t index = std::min(
                worker_times.size() - 1,
                static_cast<std::size_t>(std::ceil(
                    percentile * worker_times.size()) - 1));
            return worker_times[index] / 1000.0;
        };
        const double mean_worker_ms = worker_times.empty() ? 0.0
            : std::accumulate(
                  worker_times.begin(), worker_times.end(), 0.0) /
                worker_times.size() / 1000.0;
        std::cout << "SPiKE full IPC replay passed: frames=" << frame_count
                  << " poses=" << pose_generation
                  << " valid=" << valid_poses
                  << " worker-ms(mean/p95/max)=" << mean_worker_ms << "/"
                  << percentile_ms(0.95) << "/" << percentile_ms(1.0)
                  << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SPiKE replay failed: " << error.what() << "\n";
        return 1;
    }
}
