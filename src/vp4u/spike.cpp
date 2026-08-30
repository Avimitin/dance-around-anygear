#include "spike.h"

#include "spike_pose.h"

#include "d4xx/spike_ipc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace vp4u {

struct SpikeSource::Impl {
    std::shared_ptr<D4xxSource> cameras;
    SpikeTrackingOptions options;
    std::unique_ptr<anygear::spike::IpcHost> ipc;
    std::atomic<bool> stop{false};
    std::thread worker;
    std::mutex pose_mutex;
    std::condition_variable pose_cv;
    PoseResult pose;
    std::uint64_t generation = 0;
    std::uint64_t pose_tick_ms = 0;
    std::int64_t pose_source_time_ns = 0;
    float previous_pose[kMpLandmarkCount][3]{};
    bool previous_pose_valid[kMpLandmarkCount]{};
    bool have_depth_anchor = false;
    float depth_anchor_offset_z = 0.0f;
    std::vector<float> depth_anchor_samples;

    Impl(std::shared_ptr<D4xxSource> camera_source,
         const SpikeTrackingOptions& value)
        : cameras(std::move(camera_source)), options(value) {}

    void log(const char* format, ...) const {
        char message[768];
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(message, sizeof message, format, arguments);
        va_end(arguments);
        if (options.log) options.log(options.log_context, message);
        else {
            std::fprintf(stderr, "[anygear-spike] %s\n", message);
            std::fflush(stderr);
        }
    }

    void apply_stage_placement(PoseResult* value) {
        const float hip[3] = {
            (value->world[MP_LeftHip][0] +
             value->world[MP_RightHip][0]) * 0.5f,
            (value->world[MP_LeftHip][1] +
             value->world[MP_RightHip][1]) * 0.5f,
            (value->world[MP_LeftHip][2] +
             value->world[MP_RightHip][2]) * 0.5f,
        };
        float automatic_offset_z = 0.0f;
        if (options.auto_center_z) {
            if (!have_depth_anchor) {
                depth_anchor_samples.push_back(hip[2]);
                constexpr std::size_t sample_count = 15;
                if (depth_anchor_samples.size() >= sample_count) {
                    std::vector<float> sorted = depth_anchor_samples;
                    std::sort(sorted.begin(), sorted.end());
                    if (sorted.back() - sorted.front() <= 0.25f) {
                        const float median = sorted[sorted.size() / 2];
                        depth_anchor_offset_z = options.stage_hip_z - median;
                        have_depth_anchor = true;
                        log("placement: hip depth locked at %.3fm "
                            "(stage target %.3fm)", median,
                            options.stage_hip_z);
                    } else {
                        depth_anchor_samples.erase(
                            depth_anchor_samples.begin());
                    }
                }
            }
            automatic_offset_z = have_depth_anchor
                ? depth_anchor_offset_z
                : options.stage_hip_z - hip[2];
        }

        constexpr float degrees_to_radians =
            3.14159265358979323846f / 180.0f;
        const float pitch = options.pitch_degrees * degrees_to_radians;
        const float yaw = options.yaw_degrees * degrees_to_radians;
        const float roll = options.roll_degrees * degrees_to_radians;
        const float sin_pitch = std::sin(pitch);
        const float cos_pitch = std::cos(pitch);
        const float sin_yaw = std::sin(yaw);
        const float cos_yaw = std::cos(yaw);
        const float sin_roll = std::sin(roll);
        const float cos_roll = std::cos(roll);
        for (int index = 0; index < kMpLandmarkCount; ++index) {
            float relative[3] = {
                value->world[index][0] - hip[0],
                value->world[index][1] - hip[1],
                value->world[index][2] - hip[2],
            };
            const float pitch_y =
                cos_pitch * relative[1] - sin_pitch * relative[2];
            const float pitch_z =
                sin_pitch * relative[1] + cos_pitch * relative[2];
            relative[1] = pitch_y;
            relative[2] = pitch_z;
            const float yaw_x =
                cos_yaw * relative[0] + sin_yaw * relative[2];
            const float yaw_z =
                -sin_yaw * relative[0] + cos_yaw * relative[2];
            relative[0] = yaw_x;
            relative[2] = yaw_z;
            const float roll_x =
                cos_roll * relative[0] - sin_roll * relative[1];
            const float roll_y =
                sin_roll * relative[0] + cos_roll * relative[1];
            float stage[3] = {
                hip[0] + roll_x,
                hip[1] + roll_y,
                hip[2] + relative[2],
            };
            if (options.face_to_face) {
                stage[0] = 2.0f * options.mirror_center_x - stage[0];
                stage[2] = 2.0f * hip[2] - stage[2];
            }
            stage[0] += options.offset_x;
            stage[1] += options.offset_y;
            stage[2] += options.offset_z + automatic_offset_z;
            for (int component = 0; component < 3; ++component) {
                value->world[index][component] = stage[component];
            }
            smooth_pose_landmark(
                value, index,
                std::clamp(options.smoothing, 0.0f, 1.0f),
                previous_pose[index], &previous_pose_valid[index]);
        }
    }

    void reset_smoothing() {
        std::fill(std::begin(previous_pose_valid),
                  std::end(previous_pose_valid), false);
    }

    void run() {
        std::array<std::uint64_t, anygear::spike::kCameraCount>
            input_sequence{};
        std::uint64_t output_generation = 0;
        while (!stop) {
            bool published = false;
            for (std::size_t camera = 0;
                 camera < anygear::spike::kCameraCount; ++camera) {
                D4xxDepthFrame frame;
                if (!cameras->get_depth_frame(
                        static_cast<int>(camera), input_sequence[camera],
                        &frame)) {
                    continue;
                }
                anygear::spike::DepthInput input;
                input.frame_sequence = frame.sequence;
                input.host_time_ns = frame.host_time_ns;
                input.depth_scale_m = frame.depth_scale_m;
                input.width = frame.intrinsics.width;
                input.height = frame.intrinsics.height;
                input.principal_x = frame.intrinsics.principal_x;
                input.principal_y = frame.intrinsics.principal_y;
                input.focal_x = frame.intrinsics.focal_x;
                input.focal_y = frame.intrinsics.focal_y;
                input.distortion_model = frame.intrinsics.distortion_model;
                std::copy(std::begin(frame.intrinsics.distortion),
                          std::end(frame.intrinsics.distortion),
                          std::begin(input.distortion));
                input.device_index = frame.device_index;
                input.serial = frame.serial;
                input.depth = frame.depth.data();
                input.depth_samples = frame.depth.size();
                std::string error;
                if (!ipc->publish_depth(camera, input, &error)) {
                    log("depth publish failed: %s", error.c_str());
                    continue;
                }
                input_sequence[camera] = frame.sequence;
                published = true;
            }

            anygear::spike::PoseFrame shared_pose{};
            if (ipc->wait_for_pose(
                    output_generation, published ? 2 : 5, &shared_pose)) {
                output_generation = shared_pose.generation;
                PoseResult result;
                map_spike_pose(shared_pose, &result);
                if (result.valid) apply_stage_placement(&result);
                else reset_smoothing();
                {
                    std::lock_guard<std::mutex> lock(pose_mutex);
                    pose = result;
                    generation = output_generation;
                    pose_tick_ms = GetTickCount64();
                    pose_source_time_ns = shared_pose.source_time_ns;
                }
                pose_cv.notify_all();
            } else if (!published) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (ipc->worker_state() == anygear::spike::WorkerState::faulted) {
                log("worker fault: %s", ipc->worker_error().c_str());
                break;
            }
        }
        pose_cv.notify_all();
    }
};

std::shared_ptr<SpikeSource> SpikeSource::acquire(
    std::shared_ptr<D4xxSource> cameras,
    const SpikeTrackingOptions& options,
    std::string* error) {
    if (!cameras || cameras->active_device_count() <
            static_cast<int>(anygear::spike::kCameraCount)) {
        if (error) *error = "SPiKE requires two active D4xx depth streams";
        return {};
    }
    auto result = std::shared_ptr<SpikeSource>(
        new SpikeSource(std::move(cameras), options));
    anygear::spike::LaunchOptions launch;
    launch.executable = options.worker_executable;
    launch.arguments = options.worker_arguments;
    launch.startup_timeout_ms = options.worker_startup_timeout_ms;
    launch.log = [](void* context, const char* message) {
        static_cast<Impl*>(context)->log("ipc: %s", message);
    };
    launch.log_context = result->impl_;
    result->impl_->ipc = anygear::spike::IpcHost::launch(launch, error);
    if (!result->impl_->ipc) return {};
    result->impl_->worker = std::thread([implementation = result->impl_] {
        implementation->run();
    });
    return result;
}

SpikeSource::SpikeSource(
    std::shared_ptr<D4xxSource> cameras,
    const SpikeTrackingOptions& options)
    : impl_(new Impl(std::move(cameras), options)) {}

SpikeSource::~SpikeSource() {
    impl_->stop = true;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->ipc.reset();
    delete impl_;
}

bool SpikeSource::get_skeleton(PoseResult* output) {
    if (!output) return false;
    std::lock_guard<std::mutex> lock(impl_->pose_mutex);
    if (impl_->generation == 0 ||
        GetTickCount64() - impl_->pose_tick_ms > 2000) {
        return false;
    }
    *output = impl_->pose;
    return true;
}

bool SpikeSource::wait_for_skeleton(PoseResult* output,
                                    std::uint64_t* generation,
                                    int timeout_ms,
                                    std::int64_t* source_time_ns) {
    if (!output || !generation) return false;
    std::unique_lock<std::mutex> lock(impl_->pose_mutex);
    const bool woke = impl_->pose_cv.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeout_ms)), [&] {
            return impl_->stop || impl_->generation > *generation;
        });
    if (!woke || impl_->stop || impl_->generation <= *generation) {
        return false;
    }
    *generation = impl_->generation;
    *output = impl_->pose;
    if (source_time_ns) *source_time_ns = impl_->pose_source_time_ns;
    return GetTickCount64() - impl_->pose_tick_ms <= 2000;
}

int SpikeSource::native_width() const { return 1; }
int SpikeSource::native_height() const { return 1; }
bool SpikeSource::has_color_stream() const { return false; }

} // namespace vp4u
