// pose.cpp - MediaPipe Pose Landmarker v1 behind the VP4U pose interface.
// The official runtime is loaded dynamically; Python is not involved.
#include "pose.h"

#include "mediapipe_c_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace vp4u {

namespace {

void pose_log(const char* fmt, ...) {
    char buffer[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    std::fprintf(stderr, "[anygear-mediapipe] %s\n", buffer);
    std::fflush(stderr);
}

template <typename T>
T load_function(HMODULE module, const char* name) {
    const FARPROC raw = GetProcAddress(module, name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

} // namespace

struct PoseEngine::Impl {
    HMODULE runtime = nullptr;
    mp::PoseLandmarkerPtr landmarker = nullptr;

    mp::ErrorFreeFn error_free = nullptr;
    mp::ImageCreateFromUint8DataFn image_create = nullptr;
    mp::ImageFreeFn image_free = nullptr;
    mp::PoseLandmarkerCreateFn landmarker_create = nullptr;
    mp::PoseLandmarkerDetectForVideoFn detect_video = nullptr;
    mp::PoseLandmarkerCloseResultFn close_result = nullptr;
    mp::PoseLandmarkerCloseFn landmarker_close = nullptr;

    PoseCalib calib;
    std::vector<std::uint8_t> rgb;
    std::int64_t last_timestamp_ms = -1;
    float scale_ema = 1.0f;
    bool have_scale = false;
    bool have_previous = false;
    bool inference_error_reported = false;
    float previous_world[kMpLandmarkCount][3]{};

    std::string take_error(char* message) const {
        std::string text = message ? message : "no MediaPipe error text";
        if (message && error_free) error_free(message);
        return text;
    }

    bool load(const std::wstring& runtime_dir, const std::string& model_path,
              std::string* error) {
        const std::wstring runtime_path = runtime_dir + L"\\libmediapipe.dll";
        runtime = LoadLibraryExW(runtime_path.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!runtime) {
            *error = "LoadLibrary libmediapipe.dll failed (Win32 " +
                     std::to_string(GetLastError()) + ")";
            return false;
        }

        error_free = load_function<mp::ErrorFreeFn>(runtime, "MpErrorFree");
        image_create = load_function<mp::ImageCreateFromUint8DataFn>(
            runtime, "MpImageCreateFromUint8Data");
        image_free = load_function<mp::ImageFreeFn>(runtime, "MpImageFree");
        landmarker_create = load_function<mp::PoseLandmarkerCreateFn>(
            runtime, "MpPoseLandmarkerCreate");
        detect_video = load_function<mp::PoseLandmarkerDetectForVideoFn>(
            runtime, "MpPoseLandmarkerDetectForVideo");
        close_result = load_function<mp::PoseLandmarkerCloseResultFn>(
            runtime, "MpPoseLandmarkerCloseResult");
        landmarker_close = load_function<mp::PoseLandmarkerCloseFn>(
            runtime, "MpPoseLandmarkerClose");

        if (!error_free || !image_create || !image_free ||
            !landmarker_create || !detect_video || !close_result ||
            !landmarker_close) {
            *error = "libmediapipe.dll does not expose the MediaPipe v1 pose ABI";
            return false;
        }

        mp::PoseLandmarkerOptions options{};
        options.base_options.model_asset_path = model_path.c_str();
        options.base_options.delegate = mp::kDelegateCpu;
        options.base_options.host_environment = mp::kHostEnvironmentUnknown;
        options.base_options.host_system = mp::kHostSystemWindows;
        options.running_mode = mp::kRunningModeVideo;
        options.num_poses = 1;
        options.min_pose_detection_confidence = 0.5f;
        options.min_pose_presence_confidence = 0.5f;
        options.min_tracking_confidence = 0.5f;
        options.output_segmentation_masks = false;

        char* media_pipe_error = nullptr;
        const mp::Status status = landmarker_create(
            &options, &landmarker, &media_pipe_error);
        if (status != mp::kOk || !landmarker) {
            *error = "MpPoseLandmarkerCreate: " + take_error(media_pipe_error);
            return false;
        }

        pose_log("MediaPipe v1 CPU VIDEO task ready; model=%s", model_path.c_str());
        return true;
    }
};

PoseEngine::PoseEngine() : impl_(new Impl) {}

PoseEngine::~PoseEngine() {
    shutdown();
    delete impl_;
}

bool PoseEngine::init(const std::wstring& runtime_dir,
                      const std::string& model_path,
                      const PoseCalib& calib, std::string* error) {
    shutdown();
    impl_->calib = calib;
    return impl_->load(runtime_dir, model_path, error);
}

bool PoseEngine::ok() const {
    return impl_ && impl_->landmarker != nullptr;
}

void PoseEngine::shutdown() {
    if (!impl_) return;
    Impl& state = *impl_;
    if (state.landmarker && state.landmarker_close) {
        char* media_pipe_error = nullptr;
        state.landmarker_close(state.landmarker, &media_pipe_error);
        if (media_pipe_error && state.error_free) {
            state.error_free(media_pipe_error);
        }
        state.landmarker = nullptr;
    }
    if (state.runtime) {
        FreeLibrary(state.runtime);
        state.runtime = nullptr;
    }
    state.error_free = nullptr;
    state.image_create = nullptr;
    state.image_free = nullptr;
    state.landmarker_create = nullptr;
    state.detect_video = nullptr;
    state.close_result = nullptr;
    state.landmarker_close = nullptr;
    state.last_timestamp_ms = -1;
    state.have_scale = false;
    state.have_previous = false;
    state.inference_error_reported = false;
    state.rgb.clear();
}

bool PoseEngine::infer(const std::uint8_t* bgr, int width, int height,
                       PoseResult* output) {
    if (!output) return false;
    *output = {};
    if (!impl_ || !impl_->landmarker || !bgr || width <= 0 || height <= 0) {
        return false;
    }

    Impl& state = *impl_;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    state.rgb.resize(pixel_count * 3);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        state.rgb[index * 3 + 0] = bgr[index * 3 + 2];
        state.rgb[index * 3 + 1] = bgr[index * 3 + 1];
        state.rgb[index * 3 + 2] = bgr[index * 3 + 0];
    }

    mp::ImagePtr image = nullptr;
    char* media_pipe_error = nullptr;
    mp::Status status = state.image_create(
        mp::kImageFormatSrgb, width, height, state.rgb.data(),
        static_cast<int>(state.rgb.size()), &image, &media_pipe_error);
    if (status != mp::kOk || !image) {
        if (!state.inference_error_reported) {
            pose_log("MpImageCreateFromUint8Data failed: %s",
                     state.take_error(media_pipe_error).c_str());
            state.inference_error_reported = true;
        } else if (media_pipe_error) {
            state.error_free(media_pipe_error);
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    std::int64_t timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    if (timestamp_ms <= state.last_timestamp_ms) {
        timestamp_ms = state.last_timestamp_ms + 1;
    }
    state.last_timestamp_ms = timestamp_ms;

    mp::PoseLandmarkerResult result{};
    media_pipe_error = nullptr;
    status = state.detect_video(state.landmarker, image, nullptr, timestamp_ms,
                                &result, &media_pipe_error);
    state.image_free(image);
    if (status != mp::kOk) {
        if (!state.inference_error_reported) {
            pose_log("MpPoseLandmarkerDetectForVideo failed: %s",
                     state.take_error(media_pipe_error).c_str());
            state.inference_error_reported = true;
        } else if (media_pipe_error) {
            state.error_free(media_pipe_error);
        }
        return false;
    }

    const bool has_pose = result.pose_landmarks_count > 0 &&
        result.pose_world_landmarks_count > 0 && result.pose_landmarks &&
        result.pose_world_landmarks &&
        result.pose_landmarks[0].landmarks_count >= kMpLandmarkCount &&
        result.pose_world_landmarks[0].landmarks_count >= kMpLandmarkCount;
    if (!has_pose) {
        state.close_result(&result);
        state.have_previous = false;
        return true;
    }

    const mp::NormalizedLandmark* normalized =
        result.pose_landmarks[0].landmarks;
    const mp::Landmark* world = result.pose_world_landmarks[0].landmarks;

    const int confidence_landmarks[] = {
        MP_Nose, MP_LeftShoulder, MP_RightShoulder, MP_LeftHip,
        MP_RightHip, MP_LeftKnee, MP_RightKnee,
    };
    float confidence = 0.0f;
    for (const int index : confidence_landmarks) {
        const mp::NormalizedLandmark& landmark = normalized[index];
        float score = 1.0f;
        if (landmark.has_visibility) score = landmark.visibility;
        else if (landmark.has_presence) score = landmark.presence;
        confidence += clamp01(score);
    }
    confidence /= static_cast<float>(
        sizeof(confidence_landmarks) / sizeof(confidence_landmarks[0]));
    output->confidence = confidence;

    const auto midpoint = [&](int left, int right, int component) {
        const float* lhs = &world[left].x;
        const float* rhs = &world[right].x;
        return (lhs[component] + rhs[component]) * 0.5f;
    };
    const float hip_x = midpoint(MP_LeftHip, MP_RightHip, 0);
    const float hip_y = midpoint(MP_LeftHip, MP_RightHip, 1);
    const float hip_z = midpoint(MP_LeftHip, MP_RightHip, 2);
    const float shoulder_x = midpoint(MP_LeftShoulder, MP_RightShoulder, 0);
    const float shoulder_y = midpoint(MP_LeftShoulder, MP_RightShoulder, 1);
    const float shoulder_z = midpoint(MP_LeftShoulder, MP_RightShoulder, 2);
    const float torso_x = shoulder_x - hip_x;
    const float torso_y = shoulder_y - hip_y;
    const float torso_z = shoulder_z - hip_z;
    const float torso_length = std::sqrt(
        torso_x * torso_x + torso_y * torso_y + torso_z * torso_z);
    if (torso_length > 0.05f) {
        const float measured_scale = std::max(
            0.5f, std::min(2.0f, state.calib.torso_len_m / torso_length));
        state.scale_ema = state.have_scale
            ? state.scale_ema * 0.8f + measured_scale * 0.2f
            : measured_scale;
        state.have_scale = true;
    }
    const float scale = state.have_scale ? state.scale_ema : 1.0f;

    for (int index = 0; index < kMpLandmarkCount; ++index) {
        const mp::NormalizedLandmark& image_landmark = normalized[index];
        const mp::Landmark& world_landmark = world[index];
        output->pixel[index][0] = image_landmark.x * width;
        output->pixel[index][1] = image_landmark.y * height;
        if (image_landmark.has_visibility) {
            output->vis[index] = clamp01(image_landmark.visibility);
        } else if (image_landmark.has_presence) {
            output->vis[index] = clamp01(image_landmark.presence);
        } else {
            output->vis[index] = 1.0f;
        }

        float stage_x = state.calib.hip_stage_x +
            (world_landmark.x - hip_x) * scale;
        float stage_y = state.calib.hip_stage_y -
            (world_landmark.y - hip_y) * scale;
        float stage_z = state.calib.hip_stage_z -
            (world_landmark.z - hip_z) * scale * state.calib.z_damping;
        if (state.have_previous) {
            const float alpha = state.calib.ema_alpha;
            stage_x = alpha * stage_x +
                (1.0f - alpha) * state.previous_world[index][0];
            stage_y = alpha * stage_y +
                (1.0f - alpha) * state.previous_world[index][1];
            stage_z = alpha * stage_z +
                (1.0f - alpha) * state.previous_world[index][2];
        }
        output->world[index][0] = stage_x;
        output->world[index][1] = stage_y;
        output->world[index][2] = stage_z;
        state.previous_world[index][0] = stage_x;
        state.previous_world[index][1] = stage_y;
        state.previous_world[index][2] = stage_z;
    }

    output->valid = confidence >= 0.45f;
    state.have_previous = output->valid;
    state.inference_error_reported = false;
    state.close_result(&result);
    return true;
}

} // namespace vp4u
