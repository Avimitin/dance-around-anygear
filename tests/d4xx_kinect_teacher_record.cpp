#include "vp4u/d4xx.h"
#include "vp4u/kinect.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <librealsense2/h/rs_option.h>
#include <nlohmann/json.hpp>

#ifndef ANYGEAR_VERSION
#define ANYGEAR_VERSION "development"
#endif

namespace {

using Json = nlohmann::json;

constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;

std::wstring widen(const char* value) {
    if (!value) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), count);
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

std::int64_t steady_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

Json pose_world_json(const vp4u::PoseResult& pose) {
    Json result = Json::array();
    for (int index = 0; index < vp4u::kMpLandmarkCount; ++index) {
        result.push_back({pose.world[index][0], pose.world[index][1],
                          pose.world[index][2]});
    }
    return result;
}

Json pose_visibility_json(const vp4u::PoseResult& pose) {
    Json result = Json::array();
    for (int index = 0; index < vp4u::kMpLandmarkCount; ++index) {
        result.push_back(pose.vis[index]);
    }
    return result;
}

const char* phase_name(std::int64_t timestamp_ns,
                       std::int64_t performance_start_ns) {
    return timestamp_ns < performance_start_ns
        ? "empty-stage" : "performance";
}

struct FrameStamp {
    std::size_t frame = 0;
    std::uint64_t sequence = 0;
    std::int64_t host_time_ns = 0;
    std::int64_t sensor_time_ms = 0;
};

struct TeacherStamp {
    std::size_t frame = 0;
    std::int64_t host_time_ns = 0;
    std::int64_t sensor_time_ms = 0;
    bool valid = false;
};

struct DeviceRecording {
    std::ofstream depth;
    std::ofstream timestamps;
    std::uint64_t last_sequence = 0;
    std::vector<FrameStamp> frames;
    Json metadata;
};

struct KinectDepthRecording {
    std::ofstream depth;
    std::ofstream player_index;
    std::ofstream timestamps;
    std::uint64_t last_generation = 0;
    std::vector<FrameStamp> frames;
};

std::int64_t magnitude(std::int64_t value) {
    return value < 0 ? -value : value;
}

bool nearest_unused_frame(const std::vector<FrameStamp>& frames,
                          std::size_t first_allowed,
                          std::int64_t target_ns,
                          std::size_t* output_index) {
    if (!output_index || first_allowed >= frames.size()) return false;
    const auto first = frames.begin() +
        static_cast<std::ptrdiff_t>(first_allowed);
    const auto next = std::lower_bound(
        first, frames.end(), target_ns,
        [](const FrameStamp& frame, std::int64_t timestamp) {
            return frame.host_time_ns < timestamp;
        });
    auto best = next;
    if (next == frames.end()) {
        best = frames.end() - 1;
    } else if (next != first) {
        const auto previous = next - 1;
        if (magnitude(previous->host_time_ns - target_ns) <=
            magnitude(next->host_time_ns - target_ns)) {
            best = previous;
        }
    }
    *output_index = static_cast<std::size_t>(best - frames.begin());
    return true;
}

bool write_json_line(std::ofstream& stream, const Json& value) {
    stream << value << '\n';
    return static_cast<bool>(stream);
}

int self_test() {
    std::vector<FrameStamp> frames{
        {0, 1, 100, 10}, {1, 2, 133, 43}, {2, 3, 166, 76},
    };
    std::size_t index = 99;
    if (!nearest_unused_frame(frames, 0, 130, &index) || index != 1) {
        std::fprintf(stderr, "nearest-frame selection failed\n");
        return 1;
    }
    if (!nearest_unused_frame(frames, 2, 130, &index) || index != 2) {
        std::fprintf(stderr, "one-to-one frame selection failed\n");
        return 1;
    }
    vp4u::PoseResult pose;
    pose.valid = true;
    pose.confidence = 0.75f;
    pose.world[vp4u::MP_LeftWrist][0] = 1.25f;
    pose.vis[vp4u::MP_LeftWrist] = 1.0f;
    const Json sample = {
        {"valid", pose.valid},
        {"confidence", pose.confidence},
        {"world_m", pose_world_json(pose)},
        {"visibility", pose_visibility_json(pose)},
    };
    if (sample["world_m"].size() != vp4u::kMpLandmarkCount ||
        sample["visibility"].size() != vp4u::kMpLandmarkCount ||
        sample["world_m"][vp4u::MP_LeftWrist][0] != 1.25f ||
        std::string(phase_name(9, 10)) != "empty-stage" ||
        std::string(phase_name(10, 10)) != "performance") {
        std::fprintf(stderr, "teacher serialization failed\n");
        return 1;
    }
    std::printf("PASS: D4xx/Kinect teacher recorder self-test\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return self_test();
    }
    if (argc < 4 || argc > 7) {
        std::fprintf(stderr,
            "usage: anygear_d4xx_kinect_teacher_record <realsense2.dll> "
            "<output-directory> <seconds> [empty-stage-seconds] "
            "[warmup-seconds] [max-sync-delta-ms]\n");
        return 2;
    }

    const int seconds = std::atoi(argv[3]);
    const int empty_stage_seconds = argc >= 5 ? std::atoi(argv[4]) : 5;
    const int warmup_seconds = argc >= 6 ? std::atoi(argv[5]) : 2;
    const int max_sync_delta_ms = argc >= 7 ? std::atoi(argv[6]) : 17;
    if (seconds < 1 || seconds > 1800 || empty_stage_seconds < 0 ||
        empty_stage_seconds > seconds || warmup_seconds < 0 ||
        warmup_seconds > 30 || max_sync_delta_ms < 1 ||
        max_sync_delta_ms > 50) {
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

    constexpr int required_devices = 2;
    vp4u::D4xxOptions d4xx_options;
    d4xx_options.runtime_path = widen(argv[1]);
    d4xx_options.required_devices = required_devices;
    d4xx_options.enable_infrared = false;
    d4xx_options.align_depth_to_infrared = false;
    d4xx_options.visual_preset_by_device.assign(
        required_devices, RS2_RS400_VISUAL_PRESET_HIGH_DENSITY);
    d4xx_options.emitter_enabled_by_device.assign(required_devices, 1);
    d4xx_options.emitter_on_off_by_device.assign(required_devices, 0);

    std::string error;
    auto d4xx = vp4u::D4xxSource::acquire(d4xx_options, &error);
    if (!d4xx) {
        std::fprintf(stderr, "D4xx open failed: %s\n", error.c_str());
        return 4;
    }
    if (!d4xx->wait_until_ready(10000)) {
        std::fprintf(stderr, "D4xx streams did not produce native Z16 depth\n");
        return 5;
    }

    vp4u::KinectTrackingOptions kinect_options;
    kinect_options.use_color = false;
    kinect_options.capture_depth = true;
    auto kinect = vp4u::KinectSource::acquire(kinect_options);
    if (!kinect) {
        std::fprintf(stderr, "Kinect v1 skeleton source could not be opened\n");
        return 6;
    }

    std::fprintf(stderr,
        "warming up two D4xx depth streams and Kinect depth/skeleton for %d second(s)\n",
        warmup_seconds);
    if (warmup_seconds > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(warmup_seconds));
    }

    const int device_count = d4xx->device_count();
    if (device_count != required_devices) {
        std::fprintf(stderr, "expected exactly two active D4xx devices, got %d\n",
                     device_count);
        return 7;
    }
    const auto serials = d4xx->serial_numbers();
    std::vector<DeviceRecording> devices(
        static_cast<std::size_t>(device_count));
    for (int device_index = 0; device_index < device_count; ++device_index) {
        DeviceRecording& recording =
            devices[static_cast<std::size_t>(device_index)];
        const std::string stem = "device-" + std::to_string(device_index);
        const std::filesystem::path depth_path = output / (stem + ".z16");
        const std::filesystem::path time_path =
            output / (stem + "-timestamps.jsonl");
        recording.depth.open(depth_path, std::ios::binary | std::ios::trunc);
        recording.timestamps.open(time_path, std::ios::trunc);
        if (!recording.depth || !recording.timestamps) {
            std::fprintf(stderr, "could not create output for device %d\n",
                         device_index);
            return 8;
        }
        recording.metadata = {
            {"index", device_index},
            {"serial", serials[static_cast<std::size_t>(device_index)]},
            {"depth_file", depth_path.filename().u8string()},
            {"timestamps_file", time_path.filename().u8string()},
        };

        // Discard the most recent warm-up frame so every persisted timestamp
        // belongs to the declared capture interval.
        vp4u::D4xxDepthFrame warmup_frame;
        if (!d4xx->get_depth_frame(device_index, 0, &warmup_frame)) {
            std::fprintf(stderr,
                "D4xx device %d had no frame after warm-up\n", device_index);
            return 8;
        }
        recording.last_sequence = warmup_frame.sequence;
    }

    std::ofstream teacher_stream(output / "kinect-teacher.jsonl",
                                 std::ios::trunc);
    KinectDepthRecording kinect_depth;
    kinect_depth.depth.open(output / "kinect-depth.z16",
                            std::ios::binary | std::ios::trunc);
    kinect_depth.player_index.open(output / "kinect-player-index.u8",
                                   std::ios::binary | std::ios::trunc);
    kinect_depth.timestamps.open(output / "kinect-depth-timestamps.jsonl",
                                 std::ios::trunc);
    std::ofstream pair_stream(output / "synchronized-pairs.jsonl",
                              std::ios::trunc);
    if (!teacher_stream || !kinect_depth.depth ||
        !kinect_depth.player_index ||
        !kinect_depth.timestamps || !pair_stream) {
        std::fprintf(stderr, "could not create teacher output files\n");
        return 8;
    }

    std::uint64_t teacher_generation = 0;
    vp4u::PoseResult warmup_pose;
    std::int64_t warmup_pose_time = 0;
    std::int64_t warmup_pose_sensor_time = 0;
    std::uint32_t warmup_user_index = 0;
    if (!kinect->wait_for_skeleton(&warmup_pose, &teacher_generation, 2000,
                                   &warmup_pose_time, &warmup_user_index,
                                   nullptr, &warmup_pose_sensor_time)) {
        std::fprintf(stderr,
            "Kinect skeleton stream had no frame after warm-up\n");
        return 8;
    }
    vp4u::KinectDepthFrame warmup_depth;
    if (!kinect->wait_for_depth(&warmup_depth,
                                &kinect_depth.last_generation, 2000)) {
        std::fprintf(stderr,
            "Kinect depth stream had no frame after warm-up\n");
        return 8;
    }

    const std::int64_t started_ns = steady_time_ns();
    const std::int64_t performance_start_ns = started_ns +
        static_cast<std::int64_t>(empty_stage_seconds) * kNanosecondsPerSecond;
    const std::int64_t deadline_ns = started_ns +
        static_cast<std::int64_t>(seconds) * kNanosecondsPerSecond;
    std::fprintf(stderr,
        "capture started: keep the stage empty for %d second(s), then perform\n",
        empty_stage_seconds);

    std::vector<TeacherStamp> teacher_frames;
    std::size_t valid_teacher_frames = 0;
    bool announced_performance = empty_stage_seconds == 0;
    while (steady_time_ns() < deadline_ns) {
        bool received = false;
        const std::int64_t loop_time_ns = steady_time_ns();
        if (!announced_performance && loop_time_ns >= performance_start_ns) {
            announced_performance = true;
            std::fprintf(stderr, "empty-stage segment complete; performance segment started\n");
        }

        for (int device_index = 0; device_index < device_count; ++device_index) {
            DeviceRecording& recording =
                devices[static_cast<std::size_t>(device_index)];
            vp4u::D4xxDepthFrame frame;
            if (!d4xx->get_depth_frame(
                    device_index, recording.last_sequence, &frame)) {
                continue;
            }
            received = true;
            recording.last_sequence = frame.sequence;
            const std::size_t frame_index = recording.frames.size();
            recording.depth.write(
                reinterpret_cast<const char*>(frame.depth.data()),
                static_cast<std::streamsize>(frame.depth.size() *
                                             sizeof(std::uint16_t)));
            const Json timestamp = {
                {"frame", frame_index},
                {"sequence", frame.sequence},
                {"host_time_ns", frame.host_time_ns},
                {"phase", phase_name(frame.host_time_ns,
                                      performance_start_ns)},
            };
            if (!recording.depth ||
                !write_json_line(recording.timestamps, timestamp)) {
                std::fprintf(stderr, "D4xx write failed for device %d\n",
                             device_index);
                return 9;
            }
            recording.frames.push_back(
                {frame_index, frame.sequence, frame.host_time_ns});
            if (frame_index == 0) {
                recording.metadata["depth_scale_m"] = frame.depth_scale_m;
                recording.metadata["intrinsics"] =
                    intrinsics_json(frame.intrinsics);
                recording.metadata["frame_bytes"] =
                    frame.depth.size() * sizeof(std::uint16_t);
            }
        }

        vp4u::KinectDepthFrame depth_frame;
        if (kinect->wait_for_depth(
                &depth_frame, &kinect_depth.last_generation, 1)) {
            received = true;
            constexpr int kinect_depth_width = 320;
            constexpr int kinect_depth_height = 240;
            const std::size_t expected_values =
                (std::size_t)kinect_depth_width * kinect_depth_height;
            if (depth_frame.width != kinect_depth_width ||
                depth_frame.height != kinect_depth_height ||
                depth_frame.depth_mm.size() != expected_values ||
                depth_frame.player_index.size() != expected_values) {
                std::fprintf(stderr, "Kinect depth frame has an invalid shape\n");
                return 9;
            }
            const std::size_t frame_index = kinect_depth.frames.size();
            kinect_depth.depth.write(
                reinterpret_cast<const char*>(depth_frame.depth_mm.data()),
                static_cast<std::streamsize>(depth_frame.depth_mm.size() *
                                             sizeof(std::uint16_t)));
            kinect_depth.player_index.write(
                reinterpret_cast<const char*>(
                    depth_frame.player_index.data()),
                static_cast<std::streamsize>(
                    depth_frame.player_index.size()));
            const Json timestamp = {
                {"frame", frame_index},
                {"sequence", depth_frame.sequence},
                {"generation", depth_frame.generation},
                {"host_time_ns", depth_frame.host_time_ns},
                {"sensor_time_ms", depth_frame.sensor_time_ms},
                {"phase", phase_name(depth_frame.host_time_ns,
                                      performance_start_ns)},
            };
            if (!kinect_depth.depth || !kinect_depth.player_index ||
                !write_json_line(kinect_depth.timestamps, timestamp)) {
                std::fprintf(stderr, "Kinect depth write failed\n");
                return 9;
            }
            kinect_depth.frames.push_back(
                {frame_index, depth_frame.sequence, depth_frame.host_time_ns,
                 depth_frame.sensor_time_ms});
        }

        vp4u::PoseResult pose;
        vp4u::PoseResult prefilter_pose;
        std::int64_t pose_time_ns = 0;
        std::int64_t pose_sensor_time_ms = 0;
        std::uint32_t user_index = 0;
        if (kinect->wait_for_skeleton(
                &pose, &teacher_generation, 1, &pose_time_ns,
                &user_index, &prefilter_pose, &pose_sensor_time_ms)) {
            received = true;
            const std::size_t teacher_index = teacher_frames.size();
            const Json sample = {
                {"frame", teacher_index},
                {"generation", teacher_generation},
                {"host_time_ns", pose_time_ns},
                {"sensor_time_ms", pose_sensor_time_ms},
                {"phase", phase_name(pose_time_ns,
                                      performance_start_ns)},
                {"valid", pose.valid},
                {"confidence", pose.confidence},
                {"user_index", user_index},
                {"world_m", pose_world_json(pose)},
                {"visibility", pose_visibility_json(pose)},
                {"prefilter_valid", prefilter_pose.valid},
                {"prefilter_confidence", prefilter_pose.confidence},
                {"prefilter_world_m", pose_world_json(prefilter_pose)},
                {"prefilter_visibility", pose_visibility_json(prefilter_pose)},
            };
            if (!write_json_line(teacher_stream, sample)) {
                std::fprintf(stderr, "Kinect teacher write failed\n");
                return 9;
            }
            teacher_frames.push_back(
                {teacher_index, pose_time_ns, pose_sensor_time_ms, pose.valid});
            if (pose.valid) ++valid_teacher_frames;
        }
        if (!received) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    for (auto& recording : devices) {
        recording.depth.close();
        recording.timestamps.close();
        recording.metadata["frame_count"] = recording.frames.size();
    }
    teacher_stream.close();
    kinect_depth.depth.close();
    kinect_depth.player_index.close();
    kinect_depth.timestamps.close();

    const std::int64_t max_sync_delta_ns =
        static_cast<std::int64_t>(max_sync_delta_ms) * 1000000LL;
    std::vector<std::size_t> next_device_frame(
        static_cast<std::size_t>(device_count), 0);
    std::size_t next_kinect_depth_frame = 0;
    std::size_t pair_count = 0;
    std::size_t valid_pair_count = 0;
    for (const TeacherStamp& teacher : teacher_frames) {
        Json paired_devices = Json::array();
        std::vector<std::size_t> selected(
            static_cast<std::size_t>(device_count), 0);
        bool complete = true;
        for (int device_index = 0; device_index < device_count; ++device_index) {
            const auto& frames = devices[
                static_cast<std::size_t>(device_index)].frames;
            std::size_t selected_index = 0;
            if (!nearest_unused_frame(
                    frames,
                    next_device_frame[static_cast<std::size_t>(device_index)],
                    teacher.host_time_ns, &selected_index)) {
                complete = false;
                break;
            }
            const FrameStamp& frame = frames[selected_index];
            const std::int64_t delta_ns =
                frame.host_time_ns - teacher.host_time_ns;
            if (magnitude(delta_ns) > max_sync_delta_ns) {
                complete = false;
                break;
            }
            selected[static_cast<std::size_t>(device_index)] = selected_index;
            paired_devices.push_back({
                {"device", device_index},
                {"frame", frame.frame},
                {"sequence", frame.sequence},
                {"host_time_ns", frame.host_time_ns},
                {"delta_ns", delta_ns},
            });
        }
        if (!complete) continue;
        std::size_t selected_kinect_depth = 0;
        if (!nearest_unused_frame(
                kinect_depth.frames, next_kinect_depth_frame,
                teacher.host_time_ns, &selected_kinect_depth)) {
            continue;
        }
        const FrameStamp& kinect_depth_stamp =
            kinect_depth.frames[selected_kinect_depth];
        const std::int64_t kinect_depth_delta_ns =
            kinect_depth_stamp.host_time_ns - teacher.host_time_ns;
        if (magnitude(kinect_depth_delta_ns) > max_sync_delta_ns) continue;
        const std::int64_t kinect_sensor_delta_ms =
            kinect_depth_stamp.sensor_time_ms - teacher.sensor_time_ms;
        if (magnitude(kinect_sensor_delta_ms) > max_sync_delta_ms) continue;
        for (int device_index = 0; device_index < device_count; ++device_index) {
            next_device_frame[static_cast<std::size_t>(device_index)] =
                selected[static_cast<std::size_t>(device_index)] + 1;
        }
        next_kinect_depth_frame = selected_kinect_depth + 1;
        const Json pair = {
            {"pair", pair_count},
            {"teacher_frame", teacher.frame},
            {"teacher_host_time_ns", teacher.host_time_ns},
            {"teacher_valid", teacher.valid},
            {"phase", phase_name(teacher.host_time_ns,
                                  performance_start_ns)},
            {"kinect_depth", {
                {"frame", kinect_depth_stamp.frame},
                {"sequence", kinect_depth_stamp.sequence},
                {"host_time_ns", kinect_depth_stamp.host_time_ns},
                {"delta_ns", kinect_depth_delta_ns},
                {"sensor_time_ms", kinect_depth_stamp.sensor_time_ms},
                {"sensor_delta_ms", kinect_sensor_delta_ms},
            }},
            {"devices", std::move(paired_devices)},
        };
        if (!write_json_line(pair_stream, pair)) {
            std::fprintf(stderr, "synchronized pair write failed\n");
            return 10;
        }
        ++pair_count;
        if (teacher.valid) ++valid_pair_count;
    }
    pair_stream.close();

    Json manifest = {
        {"schema", "dance-around-anygear.d4xx-kinect-teacher.v2"},
        {"tool_version", ANYGEAR_VERSION},
        {"clock", "std::chrono::steady_clock nanoseconds"},
        {"duration_requested_s", seconds},
        {"empty_stage_s", empty_stage_seconds},
        {"warmup_s", warmup_seconds},
        {"capture_started_host_time_ns", started_ns},
        {"performance_started_host_time_ns", performance_start_ns},
        {"d4xx", {
            {"required_devices", required_devices},
            {"width", d4xx->native_width()},
            {"height", d4xx->native_height()},
            {"capture_fps", d4xx_options.fps},
            {"pixel_format", "Z16 little-endian"},
            {"coordinate", "native-depth"},
            {"infrared_stream_enabled", false},
            {"visual_preset", "high-density"},
            {"emitters", "all-on"},
            {"devices", Json::array()},
        }},
        {"teacher", {
            {"backend", "Kinect for Windows v1 skeleton"},
            {"file", "kinect-teacher.jsonl"},
            {"topology", "MediaPipe-33 compatibility landmarks"},
            {"coordinate", "Kinect camera-space meters (+X image-right, +Y up, +Z away)"},
            {"prefilter_observation", "NUI skeleton before NuiTransformSmooth and Anygear joint stabilization"},
            {"frame_count", teacher_frames.size()},
            {"valid_frame_count", valid_teacher_frames},
            {"color_stream_enabled", false},
            {"tracking", {
                {"smoothing", kinect_options.smoothing},
                {"correction", kinect_options.correction},
                {"prediction", kinect_options.prediction},
                {"jitter_radius", kinect_options.jitter_radius},
                {"max_deviation_radius",
                 kinect_options.max_deviation_radius},
                {"skeleton_lock_ms", kinect_options.skeleton_lock_ms},
                {"body_hold_ms", kinect_options.body_hold_ms},
                {"max_joint_speed_mps",
                 kinect_options.max_joint_speed_mps},
                {"bone_length_tolerance",
                 kinect_options.bone_length_tolerance},
            }},
        }},
        {"kinect_depth", {
            {"depth_file", "kinect-depth.z16"},
            {"player_index_file", "kinect-player-index.u8"},
            {"timestamps_file", "kinect-depth-timestamps.jsonl"},
            {"width", 320},
            {"height", 240},
            {"frame_bytes", 320 * 240 * (int)sizeof(std::uint16_t)},
            {"frame_count", kinect_depth.frames.size()},
            {"pixel_format", "Z16 little-endian millimetres; player index removed"},
            {"player_index_format", "U8; 0 background, 1..6 NUI player slot"},
            {"depth_scale_m", 0.001},
            {"intrinsics", {
                {"principal_x", 160.0},
                {"principal_y", 120.0},
                {"focal_x", 285.63},
                {"focal_y", 285.63},
                {"model", "Kinect SDK 1.8 nominal depth projection"},
            }},
            {"coordinate", "Kinect camera-space (+X image-right, +Y up, +Z away)"},
        }},
        {"synchronization", {
            {"file", "synchronized-pairs.jsonl"},
            {"method", "nearest monotonic one-to-one host timestamp across D4xx, Kinect depth, and Kinect skeleton"},
            {"max_delta_ms", max_sync_delta_ms},
            {"kinect_sensor_time_unit", "milliseconds since Kinect initialization"},
            {"kinect_sensor_max_delta_ms", max_sync_delta_ms},
            {"pair_count", pair_count},
            {"valid_teacher_pair_count", valid_pair_count},
        }},
    };
    for (auto& recording : devices) {
        manifest["d4xx"]["devices"].push_back(
            std::move(recording.metadata));
    }

    const std::filesystem::path manifest_path = output / "manifest.json";
    std::ofstream manifest_stream(manifest_path, std::ios::trunc);
    manifest_stream << std::setw(2) << manifest << '\n';
    if (!manifest_stream) {
        std::fprintf(stderr, "manifest write failed\n");
        return 11;
    }

    std::printf("PASS: paired D4xx/Kinect recording saved to %s\n", argv[2]);
    for (int device_index = 0; device_index < device_count; ++device_index) {
        std::printf("      D4xx[%d] frames=%zu serial=%s\n", device_index,
            devices[static_cast<std::size_t>(device_index)].frames.size(),
            serials[static_cast<std::size_t>(device_index)].c_str());
    }
    std::printf("      Kinect frames=%zu valid=%zu pairs=%zu valid-pairs=%zu\n",
        teacher_frames.size(), valid_teacher_frames, pair_count,
        valid_pair_count);
    std::printf("      Kinect depth frames=%zu\n", kinect_depth.frames.size());
    return 0;
}
