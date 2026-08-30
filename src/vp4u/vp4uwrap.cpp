// VP4U compatibility implementation for external pose sensors.
#include "vp4u_abi.h"
#include "body_prediction.h"
#include "webcam.h"
#include "pose.h"
#include "kinect.h"
#include "steamvr.h"
#include "d4xx.h"
#include "spike.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

namespace vp4u {

// ---------------------------------------------------------------- logger ----
struct LoggerEntry { OnLogCallback cb; void* ctx; };
static std::mutex g_logMtx;
static std::vector<LoggerEntry> g_loggers; // handle = index+1; 0 = invalid

static intptr_t logger_add(OnLogCallback cb, void* ctx) {
    if (!cb) return 0;
    std::lock_guard<std::mutex> lk(g_logMtx);
    for (size_t i = 0; i < g_loggers.size(); i++) {
        if (g_loggers[i].cb == cb && g_loggers[i].ctx == ctx) {
            return (intptr_t)(i + 1);
        }
    }
    for (size_t i = 0; i < g_loggers.size(); i++) {
        if (!g_loggers[i].cb) { g_loggers[i] = {cb, ctx}; return (intptr_t)(i + 1); }
    }
    g_loggers.push_back({cb, ctx});
    return (intptr_t)g_loggers.size();
}

static void logger_remove(intptr_t h) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (h >= 1 && (size_t)h <= g_loggers.size()) g_loggers[(size_t)h - 1] = {nullptr, nullptr};
}

static void vlog(const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    std::lock_guard<std::mutex> lk(g_logMtx);
    for (auto& e : g_loggers) {
        if (e.cb) e.cb(e.ctx, buf);
    }
}

static void log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- config ----
struct Config {
#if defined(ANYGEAR_BACKEND_KINECT)
    int capture_w = 848, capture_h = 480, capture_fps = 30;
#else
    int capture_w = 848, capture_h = 480, capture_fps = 30;
#endif
    // Vp4uWrap custom section (all optional)
    int   camera_index = 0;
#if defined(ANYGEAR_BACKEND_KINECT)
    int   preview_fps = 15;
    int   image_pool_max = 12;
#else
    int   preview_fps = 15;
    int   image_pool_max = 12;
#endif
    bool  mirror_x = false;
    // Kinect v1 NUI skeleton +X projects toward increasing depth-image X.
    // This compatibility view transform is applied before the optional
    // face-to-face stage orientation; it is independent of sensor calibration.
    bool  kinect_flip_x = true;
    // The performer and on-screen avatar face each other while movement stays
    // mirrored in screen space. Rotate around the configured horizontal center
    // and around the live hip in depth.
    bool  kinect_face_to_face = true;
    float kinect_mirror_center_x = 0.55f;
    bool  kinect_use_color = false;
    // The callback contract requires a non-null main image with valid 3D
    // bodies. False uses a 1x1 sentinel rather than copying two
    // 640x480 images into every hardware-skeleton callback.
    bool  kinect_pose_images = false;
    float kinect_offset_x = 0.0f;
    float kinect_offset_y = 0.0f;
    float kinect_offset_z = 0.0f;
    float kinect_smoothing = 0.35f;
    float kinect_correction = 0.65f;
    float kinect_prediction = 0.00f;
    float kinect_jitter_radius = 0.05f;
    float kinect_max_deviation_radius = 0.04f;
    int   kinect_skeleton_lock_ms = 400;
    int   kinect_body_hold_ms = 500;
    float kinect_max_joint_speed_mps = 8.0f;
    float kinect_bone_length_tolerance = 0.35f;
    bool  steamvr_auto_center = true;
    bool  steamvr_face_to_face = true;
    bool  steamvr_require_waist = true;
    bool  steamvr_require_feet = true;
    int   steamvr_body_hold_ms = 250;
    float steamvr_stage_hip_x = 0.55f;
    float steamvr_stage_hip_y = 0.0f;
    float steamvr_stage_hip_z = 2.30f;
    std::string steamvr_runtime_dir;
    int   d4xx_primary_device = 0;
    std::string d4xx_primary_serial;
    int   d4xx_required_devices = 2;
    int   d4xx_infrared_index = 1;
    int   d4xx_frame_stall_timeout_ms = 1500;
    int   d4xx_reconnect_delay_ms = 500;
    std::string d4xx_emitter_mode = "all-on";
    float d4xx_depth_scale_m = 0.001f;
    bool  d4xx_face_to_face = true;
    bool  d4xx_auto_center_z = true;
    float d4xx_stage_hip_z = 2.30f;
    float d4xx_mirror_center_x = 0.55f;
    float d4xx_offset_x = 0.0f;
    float d4xx_offset_y = 0.0f;
    float d4xx_offset_z = 0.0f;
    float d4xx_pitch_degrees = 0.0f;
    float d4xx_yaw_degrees = 0.0f;
    float d4xx_roll_degrees = 0.0f;
    float d4xx_smoothing = 0.65f;
    int   d4xx_body_hold_ms = 250;
    int   d4xx_output_fps = 30;
    float d4xx_prediction_ms = 20.0f;
    std::string d4xx_runtime_path;
    std::string spike_worker_executable;
    bool  spike_python_module = false;
    std::string spike_model_path;
    std::string spike_worker_log_path;
    int   spike_startup_timeout_ms = 45000;
    float spike_smoothing = 1.0f;
    float hip_stage_x = 0.9f, hip_stage_y = 1.0f, hip_stage_z = 0.75f;
    float torso_len_m = 0.52f;
    float z_damping = 0.7f;
    float ema_alpha = 0.55f;
    std::string model_path;   // default: <dll>/dance_around_anygear_webcam/*.task
    std::string runtime_dir;  // default: <dll>/dance_around_anygear_webcam
};

static Config g_cfg;
static std::string g_cfgPath;
static std::string dll_dir_w();

static void config_apply_environment() {
    const auto apply_int = [](const char *name, int min_value, int max_value, int *target) {
        const char *text = std::getenv(name);
        if (!text || !*text) return;
        char *end = nullptr;
        const long value = std::strtol(text, &end, 10);
        if (end == text || *end != '\0' || value < min_value || value > max_value) {
            log("config: ignoring invalid %s='%s' (expected %d..%d)",
                name, text, min_value, max_value);
            return;
        }
        *target = (int)value;
    };
    const auto apply_float = [](const char* name, float min_value,
                                float max_value, float* target) {
        const char* text = std::getenv(name);
        if (!text || !*text) return;
        char* end = nullptr;
        const double value = std::strtod(text, &end);
        if (end == text || *end != '\0' || !std::isfinite(value) ||
            value < min_value || value > max_value) {
            log("config: ignoring invalid %s='%s' (expected %.2f..%.2f)",
                name, text, min_value, max_value);
            return;
        }
        *target = static_cast<float>(value);
    };
    const auto apply_bool = [](const char* name, bool* target) {
        const char* text = std::getenv(name);
        if (!text || !*text) return;
        if (std::strcmp(text, "1") == 0 || _stricmp(text, "true") == 0) {
            *target = true;
        } else if (std::strcmp(text, "0") == 0 ||
                   _stricmp(text, "false") == 0) {
            *target = false;
        } else {
            log("config: ignoring invalid %s='%s' (expected true/false)",
                name, text);
        }
    };

#if defined(ANYGEAR_BACKEND_WEBCAM) || defined(ANYGEAR_BACKEND_D4XX) || \
    defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    apply_int("VP4U_CAPTURE_FPS", 1, 30, &g_cfg.capture_fps);
#else
    apply_int("VP4U_CAPTURE_FPS", 1, 60, &g_cfg.capture_fps);
#endif
    apply_int("VP4U_PREVIEW_FPS", 1, 30, &g_cfg.preview_fps);
    apply_int("VP4U_IMAGE_POOL_MAX", 4, 24, &g_cfg.image_pool_max);
    apply_int("VP4U_CAMERA_INDEX", 0, 31, &g_cfg.camera_index);
    apply_int("VP4U_D4XX_PRIMARY_DEVICE", 0, 31,
              &g_cfg.d4xx_primary_device);
    apply_int("VP4U_D4XX_REQUIRED_DEVICES", 1, 8,
              &g_cfg.d4xx_required_devices);
    apply_int("VP4U_D4XX_INFRARED_INDEX", 1, 2,
              &g_cfg.d4xx_infrared_index);
    apply_int("VP4U_D4XX_BODY_HOLD_MS", 0, 2000,
              &g_cfg.d4xx_body_hold_ms);
    apply_int("VP4U_D4XX_OUTPUT_FPS", 15, 60,
              &g_cfg.d4xx_output_fps);
    apply_float("VP4U_D4XX_PREDICTION_MS", 0.0f, 100.0f,
                &g_cfg.d4xx_prediction_ms);
    apply_float("VP4U_D4XX_OFFSET_X", -5.0f, 5.0f,
                &g_cfg.d4xx_offset_x);
    apply_float("VP4U_D4XX_OFFSET_Y", -5.0f, 5.0f,
                &g_cfg.d4xx_offset_y);
    apply_float("VP4U_D4XX_OFFSET_Z", -5.0f, 5.0f,
                &g_cfg.d4xx_offset_z);
    apply_bool("VP4U_D4XX_AUTO_CENTER_Z", &g_cfg.d4xx_auto_center_z);
    apply_float("VP4U_D4XX_STAGE_HIP_Z", 0.5f, 5.0f,
                &g_cfg.d4xx_stage_hip_z);
    apply_float("VP4U_D4XX_PITCH_DEGREES", -90.0f, 90.0f,
                &g_cfg.d4xx_pitch_degrees);
    apply_float("VP4U_D4XX_YAW_DEGREES", -180.0f, 180.0f,
                &g_cfg.d4xx_yaw_degrees);
    apply_float("VP4U_D4XX_ROLL_DEGREES", -180.0f, 180.0f,
                &g_cfg.d4xx_roll_degrees);
}

using Json = nlohmann::json;

static void json_assign_integer(const Json& object, const char* key,
                                int* target) {
    const auto value = object.find(key);
    if (value == object.end()) return;
    if (!value->is_number_integer()) {
        log("config: %s must be an integer; ignored", key);
        return;
    }
    try {
        *target = value->get<int>();
    } catch (const std::exception& error) {
        log("config: invalid %s: %s", key, error.what());
    }
}

static void json_assign_number(const Json& object, const char* key,
                               float* target) {
    const auto value = object.find(key);
    if (value == object.end()) return;
    if (!value->is_number()) {
        log("config: %s must be a number; ignored", key);
        return;
    }
    try {
        *target = value->get<float>();
    } catch (const std::exception& error) {
        log("config: invalid %s: %s", key, error.what());
    }
}

static void json_assign_boolean(const Json& object, const char* key,
                                bool* target) {
    const auto value = object.find(key);
    if (value == object.end()) return;
    if (value->is_boolean()) {
        *target = value->get<bool>();
        return;
    }
    // Continue accepting the old Vp4uWrap convention for compatibility.
    if (value->is_number_integer()) {
        *target = value->get<int>() != 0;
        return;
    }
    log("config: %s must be a boolean; ignored", key);
}

static void json_assign_string(const Json& object, const char* key,
                               std::string* target) {
    const auto value = object.find(key);
    if (value == object.end()) return;
    if (!value->is_string()) {
        log("config: %s must be a string; ignored", key);
        return;
    }
    *target = value->get<std::string>();
}

#if defined(ANYGEAR_BACKEND_D4XX) || defined(ANYGEAR_BACKEND_D4XX_SPIKE)
static void config_apply_d4xx_placement(const Json& section) {
    json_assign_integer(section, "PrimaryDevice", &g_cfg.d4xx_primary_device);
    json_assign_string(section, "PrimarySerial", &g_cfg.d4xx_primary_serial);
    json_assign_integer(section, "RequiredDevices", &g_cfg.d4xx_required_devices);
    json_assign_integer(section, "InfraredIndex", &g_cfg.d4xx_infrared_index);
    json_assign_integer(section, "FrameStallTimeoutMs",
                        &g_cfg.d4xx_frame_stall_timeout_ms);
    json_assign_integer(section, "ReconnectDelayMs",
                        &g_cfg.d4xx_reconnect_delay_ms);
    json_assign_string(section, "EmitterMode", &g_cfg.d4xx_emitter_mode);
    json_assign_boolean(section, "FaceToFace", &g_cfg.d4xx_face_to_face);
    json_assign_boolean(section, "AutoCenterZ", &g_cfg.d4xx_auto_center_z);
    json_assign_number(section, "StageHipZ", &g_cfg.d4xx_stage_hip_z);
    json_assign_number(section, "MirrorCenterX", &g_cfg.d4xx_mirror_center_x);
    json_assign_number(section, "OffsetX", &g_cfg.d4xx_offset_x);
    json_assign_number(section, "OffsetY", &g_cfg.d4xx_offset_y);
    json_assign_number(section, "OffsetZ", &g_cfg.d4xx_offset_z);
    json_assign_number(section, "PitchDegrees", &g_cfg.d4xx_pitch_degrees);
    json_assign_number(section, "YawDegrees", &g_cfg.d4xx_yaw_degrees);
    json_assign_number(section, "RollDegrees", &g_cfg.d4xx_roll_degrees);
#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    json_assign_number(section, "Smoothing", &g_cfg.spike_smoothing);
#else
    json_assign_number(section, "Smoothing", &g_cfg.d4xx_smoothing);
#endif
    json_assign_integer(section, "BodyHoldMs", &g_cfg.d4xx_body_hold_ms);
    json_assign_integer(section, "OutputFps", &g_cfg.d4xx_output_fps);
    json_assign_number(section, "PredictionMs", &g_cfg.d4xx_prediction_ms);
}
#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
static void config_apply_spike_worker(const Json& section) {
    json_assign_string(section, "Executable", &g_cfg.spike_worker_executable);
    json_assign_boolean(section, "PythonModule", &g_cfg.spike_python_module);
    json_assign_string(section, "Model", &g_cfg.spike_model_path);
    json_assign_string(section, "Log", &g_cfg.spike_worker_log_path);
    json_assign_integer(section, "StartupTimeoutMs",
                        &g_cfg.spike_startup_timeout_ms);
}
#endif
#endif

static bool read_config_text(const std::string& path, std::string* output) {
    if (!output) return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    output->clear();
    if (length > 0 && length < 1024 * 1024) {
        output->resize(static_cast<std::size_t>(length));
        const std::size_t read = std::fread(
            output->data(), 1, static_cast<std::size_t>(length), file);
        output->resize(read);
    }
    std::fclose(file);
    return length >= 0 && length < 1024 * 1024;
}

static void config_apply_local_d4xx_file() {
#if defined(ANYGEAR_BACKEND_D4XX) || defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    const std::string path =
        dll_dir_w() +
#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
        "\\dance_around_anygear_d4xx_spike.json";
#else
        "\\dance_around_anygear_d4xx_mediapipe_experimental.json";
#endif
    std::string json;
    if (!read_config_text(path, &json)) {
        log("placement config: '%s' absent; using built-in defaults", path.c_str());
        return;
    }
    const Json document = Json::parse(json, nullptr, false);
    if (document.is_discarded()) {
        log("placement config: '%s' is invalid JSON; ignored", path.c_str());
        return;
    }
    const auto section = document.find("D4xxPlacement");
    if (section == document.end() || !section->is_object()) {
        log("placement config: '%s' requires a D4xxPlacement object; ignored",
            path.c_str());
        return;
    }
    config_apply_d4xx_placement(*section);
#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    const auto worker = document.find("Worker");
    if (worker != document.end() && worker->is_object()) {
        config_apply_spike_worker(*worker);
    }
#endif
    log("placement config: loaded '%s'", path.c_str());
#endif
}

static std::string normalize_host_json(const std::string& input) {
    // The cabinet configuration is JSON-like but contains trailing commas.
    // Normalize only that known extension; nlohmann/json still performs all
    // structural parsing, escaping, type checks, and error handling.
    std::string output;
    output.reserve(input.size());
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const char value = input[index];
        if (in_string) {
            output.push_back(value);
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') in_string = false;
            continue;
        }
        if (value == '"') {
            in_string = true;
            output.push_back(value);
            continue;
        }
        if (value == ',') {
            std::size_t next = index + 1;
            while (next < input.size() && std::isspace(
                       static_cast<unsigned char>(input[next]))) {
                ++next;
            }
            if (next < input.size() &&
                (input[next] == '}' || input[next] == ']')) {
                continue;
            }
        }
        output.push_back(value);
    }
    return output;
}

static void config_apply_json(const std::string& js) {
    const Json document = Json::parse(
        normalize_host_json(js), nullptr, false, true);
    if (document.is_discarded() || !document.is_object()) {
        log("LoadConfig: host configuration is invalid JSON; ignored");
        return;
    }
    json_assign_integer(document, "CaptureWidth", &g_cfg.capture_w);
    json_assign_integer(document, "CaptureHeight", &g_cfg.capture_h);
    json_assign_integer(document, "CaptureFrameRate", &g_cfg.capture_fps);
    const auto real_sense = document.find("RealSenseInputSettings");
    if (real_sense != document.end() && real_sense->is_object()) {
        const auto camera = real_sense->find("CameraConfig");
        if (camera != real_sense->end() && camera->is_object()) {
            json_assign_integer(*camera, "CaptureWidth", &g_cfg.capture_w);
            json_assign_integer(*camera, "CaptureHeight", &g_cfg.capture_h);
            json_assign_integer(*camera, "CaptureFrameRate", &g_cfg.capture_fps);
        }
    }

    // Project-specific keys are scoped so vendor fields with the same name
    // cannot be consumed accidentally.
    const auto section = document.find("Vp4uWrap");
    if (section == document.end() || !section->is_object()) return;
    const Json& object = *section;
    json_assign_integer(object, "CameraIndex", &g_cfg.camera_index);
    json_assign_integer(object, "PreviewFps", &g_cfg.preview_fps);
    json_assign_integer(object, "ImagePoolMax", &g_cfg.image_pool_max);
    json_assign_number(object, "HipStageX", &g_cfg.hip_stage_x);
    json_assign_number(object, "HipStageY", &g_cfg.hip_stage_y);
    json_assign_number(object, "HipStageZ", &g_cfg.hip_stage_z);
    json_assign_number(object, "TorsoLenM", &g_cfg.torso_len_m);
    json_assign_number(object, "ZDamping", &g_cfg.z_damping);
    json_assign_number(object, "EmaAlpha", &g_cfg.ema_alpha);
    json_assign_boolean(object, "KinectFlipX", &g_cfg.kinect_flip_x);
    json_assign_boolean(object, "KinectFaceToFace", &g_cfg.kinect_face_to_face);
    json_assign_number(object, "KinectMirrorCenterX", &g_cfg.kinect_mirror_center_x);
    json_assign_boolean(object, "KinectUseColor", &g_cfg.kinect_use_color);
    json_assign_boolean(object, "KinectPoseImages", &g_cfg.kinect_pose_images);
    json_assign_number(object, "KinectOffsetX", &g_cfg.kinect_offset_x);
    json_assign_number(object, "KinectOffsetY", &g_cfg.kinect_offset_y);
    json_assign_number(object, "KinectOffsetZ", &g_cfg.kinect_offset_z);
    json_assign_number(object, "KinectSmoothing", &g_cfg.kinect_smoothing);
    json_assign_number(object, "KinectCorrection", &g_cfg.kinect_correction);
    json_assign_number(object, "KinectPrediction", &g_cfg.kinect_prediction);
    json_assign_number(object, "KinectJitterRadius", &g_cfg.kinect_jitter_radius);
    json_assign_number(object, "KinectMaxDeviationRadius", &g_cfg.kinect_max_deviation_radius);
    json_assign_integer(object, "KinectSkeletonLockMs", &g_cfg.kinect_skeleton_lock_ms);
    json_assign_integer(object, "KinectBodyHoldMs", &g_cfg.kinect_body_hold_ms);
    json_assign_number(object, "KinectMaxJointSpeedMps", &g_cfg.kinect_max_joint_speed_mps);
    json_assign_number(object, "KinectBoneLengthTolerance", &g_cfg.kinect_bone_length_tolerance);
    json_assign_boolean(object, "SteamVrAutoCenter", &g_cfg.steamvr_auto_center);
    json_assign_boolean(object, "SteamVrFaceToFace", &g_cfg.steamvr_face_to_face);
    json_assign_boolean(object, "SteamVrRequireWaist", &g_cfg.steamvr_require_waist);
    json_assign_boolean(object, "SteamVrRequireFeet", &g_cfg.steamvr_require_feet);
    json_assign_integer(object, "SteamVrBodyHoldMs", &g_cfg.steamvr_body_hold_ms);
    json_assign_number(object, "SteamVrStageHipX", &g_cfg.steamvr_stage_hip_x);
    json_assign_number(object, "SteamVrStageHipY", &g_cfg.steamvr_stage_hip_y);
    json_assign_number(object, "SteamVrStageHipZ", &g_cfg.steamvr_stage_hip_z);
    json_assign_integer(object, "D4xxPrimaryDevice", &g_cfg.d4xx_primary_device);
    json_assign_string(object, "D4xxPrimarySerial", &g_cfg.d4xx_primary_serial);
    json_assign_integer(object, "D4xxRequiredDevices", &g_cfg.d4xx_required_devices);
    json_assign_integer(object, "D4xxInfraredIndex", &g_cfg.d4xx_infrared_index);
    json_assign_integer(object, "D4xxFrameStallTimeoutMs",
                        &g_cfg.d4xx_frame_stall_timeout_ms);
    json_assign_integer(object, "D4xxReconnectDelayMs",
                        &g_cfg.d4xx_reconnect_delay_ms);
    json_assign_number(object, "D4xxDepthScaleM", &g_cfg.d4xx_depth_scale_m);
    json_assign_boolean(object, "D4xxFaceToFace", &g_cfg.d4xx_face_to_face);
    json_assign_boolean(object, "D4xxAutoCenterZ", &g_cfg.d4xx_auto_center_z);
    json_assign_number(object, "D4xxStageHipZ", &g_cfg.d4xx_stage_hip_z);
    json_assign_number(object, "D4xxMirrorCenterX", &g_cfg.d4xx_mirror_center_x);
    json_assign_number(object, "D4xxOffsetX", &g_cfg.d4xx_offset_x);
    json_assign_number(object, "D4xxOffsetY", &g_cfg.d4xx_offset_y);
    json_assign_number(object, "D4xxOffsetZ", &g_cfg.d4xx_offset_z);
    json_assign_number(object, "D4xxPitchDegrees", &g_cfg.d4xx_pitch_degrees);
    json_assign_number(object, "D4xxYawDegrees", &g_cfg.d4xx_yaw_degrees);
    json_assign_number(object, "D4xxRollDegrees", &g_cfg.d4xx_roll_degrees);
    json_assign_number(object, "D4xxSmoothing", &g_cfg.d4xx_smoothing);
    json_assign_integer(object, "D4xxBodyHoldMs", &g_cfg.d4xx_body_hold_ms);
    json_assign_integer(object, "D4xxOutputFps", &g_cfg.d4xx_output_fps);
    json_assign_number(object, "D4xxPredictionMs", &g_cfg.d4xx_prediction_ms);
    json_assign_string(object, "ModelPath", &g_cfg.model_path);
    json_assign_string(object, "RuntimeDir", &g_cfg.runtime_dir);
    json_assign_string(object, "SteamVrRuntimeDir", &g_cfg.steamvr_runtime_dir);
    json_assign_string(object, "D4xxRuntimePath", &g_cfg.d4xx_runtime_path);
    json_assign_boolean(object, "MirrorX", &g_cfg.mirror_x);
}

static void config_load_file(const char* path) {
    std::string js;
    if (path && *path) {
        g_cfgPath = path;
        if (read_config_text(path, &js)) {
            config_apply_json(js);
        } else {
            log("LoadConfig: cannot open '%s', using backend defaults", path);
        }
    } else {
        log("LoadConfig: empty host path, using backend defaults");
    }
    // The plugin-local placement profile intentionally wins over the game's
    // vendor configuration. This keeps machine-specific sensor placement out
    // of original game files and out of launch scripts.
    config_apply_local_d4xx_file();
#if defined(ANYGEAR_BACKEND_KINECT)
    // Release-safe Kinect pacing belongs to the backend target, not to a user
    // launch script or the vendor JSON. Maintainers can still override these
    // three bounded values with the documented environment variables.
    g_cfg.capture_fps = 30;
    g_cfg.preview_fps = 15;
    g_cfg.image_pool_max = 12;
#endif
    config_apply_environment();
#if defined(ANYGEAR_BACKEND_WEBCAM) || defined(ANYGEAR_BACKEND_D4XX) || \
    defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    g_cfg.capture_fps = std::clamp(g_cfg.capture_fps, 1, 30);
#else
    g_cfg.capture_fps = std::clamp(g_cfg.capture_fps, 1, 60);
#endif
    g_cfg.preview_fps = std::clamp(g_cfg.preview_fps, 1, 30);
    g_cfg.image_pool_max = std::clamp(g_cfg.image_pool_max, 4, 24);
#if defined(ANYGEAR_BACKEND_WEBCAM) || defined(ANYGEAR_BACKEND_D4XX) || \
    defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    // The vendor JSON may request the native D435 image size. Monocular pose
    // does not benefit from submitting more than the validated webcam frame,
    // so keep inference/callback allocations bounded inside the DLL.
    g_cfg.capture_w = std::clamp(g_cfg.capture_w, 160, 848);
    g_cfg.capture_h = std::clamp(g_cfg.capture_h, 120, 480);
#endif
#if defined(ANYGEAR_BACKEND_D4XX) || defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    // This is the native D430 profile validated by the standalone probe. Keep
    // the two physical streams and MediaPipe input on one fixed pixel grid.
    g_cfg.capture_w = 848;
    g_cfg.capture_h = 480;
    g_cfg.d4xx_primary_device = std::clamp(
        g_cfg.d4xx_primary_device, 0, 31);
    g_cfg.d4xx_required_devices = std::clamp(
        g_cfg.d4xx_required_devices, 1, 8);
    g_cfg.d4xx_infrared_index = std::clamp(
        g_cfg.d4xx_infrared_index, 1, 2);
    g_cfg.d4xx_frame_stall_timeout_ms = std::clamp(
        g_cfg.d4xx_frame_stall_timeout_ms, 100, 60000);
    g_cfg.d4xx_reconnect_delay_ms = std::clamp(
        g_cfg.d4xx_reconnect_delay_ms, 50, 60000);
    g_cfg.d4xx_depth_scale_m = std::clamp(
        g_cfg.d4xx_depth_scale_m, 0.000001f, 0.1f);
    g_cfg.d4xx_offset_x = std::clamp(g_cfg.d4xx_offset_x, -5.0f, 5.0f);
    g_cfg.d4xx_offset_y = std::clamp(g_cfg.d4xx_offset_y, -5.0f, 5.0f);
    g_cfg.d4xx_offset_z = std::clamp(g_cfg.d4xx_offset_z, -5.0f, 5.0f);
    g_cfg.d4xx_stage_hip_z = std::clamp(
        g_cfg.d4xx_stage_hip_z, 0.5f, 5.0f);
    g_cfg.d4xx_pitch_degrees = std::clamp(
        g_cfg.d4xx_pitch_degrees, -90.0f, 90.0f);
    g_cfg.d4xx_yaw_degrees = std::clamp(
        g_cfg.d4xx_yaw_degrees, -180.0f, 180.0f);
    g_cfg.d4xx_roll_degrees = std::clamp(
        g_cfg.d4xx_roll_degrees, -180.0f, 180.0f);
    g_cfg.d4xx_smoothing = std::clamp(g_cfg.d4xx_smoothing, 0.0f, 1.0f);
    g_cfg.d4xx_body_hold_ms = std::clamp(g_cfg.d4xx_body_hold_ms, 0, 2000);
    g_cfg.d4xx_output_fps = std::clamp(g_cfg.d4xx_output_fps, 15, 60);
    g_cfg.d4xx_prediction_ms = std::clamp(
        g_cfg.d4xx_prediction_ms, 0.0f, 100.0f);
#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    std::transform(
        g_cfg.d4xx_emitter_mode.begin(), g_cfg.d4xx_emitter_mode.end(),
        g_cfg.d4xx_emitter_mode.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    static constexpr const char* emitter_modes[] = {
        "all-on", "all-off", "first-only", "second-only", "alternating",
    };
    if (std::find(
            std::begin(emitter_modes), std::end(emitter_modes),
            g_cfg.d4xx_emitter_mode) == std::end(emitter_modes)) {
        log("config: unsupported D4xxPlacement.EmitterMode '%s'; using all-on",
            g_cfg.d4xx_emitter_mode.c_str());
        g_cfg.d4xx_emitter_mode = "all-on";
    }
    g_cfg.spike_smoothing = std::clamp(g_cfg.spike_smoothing, 0.0f, 1.0f);
    g_cfg.spike_startup_timeout_ms = std::clamp(
        g_cfg.spike_startup_timeout_ms, 5000, 120000);
#endif
#endif
    g_cfg.kinect_smoothing = std::clamp(g_cfg.kinect_smoothing, 0.0f, 1.0f);
    g_cfg.kinect_correction = std::clamp(g_cfg.kinect_correction, 0.0f, 1.0f);
    g_cfg.kinect_prediction = std::clamp(g_cfg.kinect_prediction, 0.0f, 1.0f);
    g_cfg.kinect_jitter_radius = std::clamp(g_cfg.kinect_jitter_radius, 0.0f, 1.0f);
    g_cfg.kinect_max_deviation_radius = std::clamp(
        g_cfg.kinect_max_deviation_radius, 0.0f, 1.0f);
    g_cfg.kinect_skeleton_lock_ms = std::clamp(g_cfg.kinect_skeleton_lock_ms, 0, 2000);
    g_cfg.kinect_body_hold_ms = std::clamp(g_cfg.kinect_body_hold_ms, 0, 2000);
    g_cfg.kinect_max_joint_speed_mps = std::clamp(
        g_cfg.kinect_max_joint_speed_mps, 1.0f, 20.0f);
    g_cfg.kinect_bone_length_tolerance = std::clamp(
        g_cfg.kinect_bone_length_tolerance, 0.10f, 1.00f);
    g_cfg.steamvr_body_hold_ms = std::clamp(
        g_cfg.steamvr_body_hold_ms, 0, 2000);
#if defined(ANYGEAR_BACKEND_KINECT)
    log("runtime config: Kinect stable profile %d Hz, preview %d Hz, pool %d, "
        "no-color=%d, face-to-face=%d, ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.preview_fps, g_cfg.image_pool_max,
        (int)!g_cfg.kinect_use_color, (int)g_cfg.kinect_face_to_face,
        sizeof(ParsedBody));
#elif defined(ANYGEAR_BACKEND_STEAMVR)
    log("runtime config: SteamVR tracked-pose profile %d Hz, preview %d Hz, "
        "pool %d, strict-waist=%d, strict-feet=%d, face-to-face=%d, "
        "ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.preview_fps, g_cfg.image_pool_max,
        (int)g_cfg.steamvr_require_waist, (int)g_cfg.steamvr_require_feet,
        (int)g_cfg.steamvr_face_to_face, sizeof(ParsedBody));
#elif defined(ANYGEAR_BACKEND_D4XX)
    log("runtime config: D4xx depth+IR profile %d Hz, preview %d Hz, "
        "pool %d, primary=%d, required=%d, IR-index=%d, frame=%dx%d, "
        "stall=%dms, reconnect=%dms, "
        "face-to-face=%d, auto-center-z=%d@%.2fm, "
        "offset=(%.2f,%.2f,%.2f), rotation=(%.1f,%.1f,%.1f)deg, "
        "smooth=%.2f, hold=%dms, output=%dHz, prediction=%.0fms, "
        "ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.preview_fps, g_cfg.image_pool_max,
        g_cfg.d4xx_primary_device, g_cfg.d4xx_required_devices,
        g_cfg.d4xx_infrared_index, g_cfg.capture_w, g_cfg.capture_h,
        g_cfg.d4xx_frame_stall_timeout_ms, g_cfg.d4xx_reconnect_delay_ms,
        (int)g_cfg.d4xx_face_to_face, (int)g_cfg.d4xx_auto_center_z,
        g_cfg.d4xx_stage_hip_z, g_cfg.d4xx_offset_x,
        g_cfg.d4xx_offset_y, g_cfg.d4xx_offset_z,
        g_cfg.d4xx_pitch_degrees, g_cfg.d4xx_yaw_degrees,
        g_cfg.d4xx_roll_degrees,
        g_cfg.d4xx_smoothing, g_cfg.d4xx_body_hold_ms,
        g_cfg.d4xx_output_fps, g_cfg.d4xx_prediction_ms,
        sizeof(ParsedBody));
#elif defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    log("runtime config: D4xx/SPiKE depth-only profile %d Hz, "
        "devices=%d, frame=%dx%d, stall=%dms, reconnect=%dms, emitter=%s, "
        "face-to-face=%d, "
        "auto-center-z=%d@%.2fm, smooth=%.2f, hold=%dms, "
        "output=%dHz, prediction=%.0fms, worker-timeout=%dms, "
        "ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.d4xx_required_devices,
        g_cfg.capture_w, g_cfg.capture_h,
        g_cfg.d4xx_frame_stall_timeout_ms, g_cfg.d4xx_reconnect_delay_ms,
        g_cfg.d4xx_emitter_mode.c_str(),
        (int)g_cfg.d4xx_face_to_face,
        (int)g_cfg.d4xx_auto_center_z, g_cfg.d4xx_stage_hip_z,
        g_cfg.spike_smoothing, g_cfg.d4xx_body_hold_ms,
        g_cfg.d4xx_output_fps, g_cfg.d4xx_prediction_ms,
        g_cfg.spike_startup_timeout_ms, sizeof(ParsedBody));
#else
    log("runtime config: MediaPipe webcam profile %d Hz, preview %d Hz, "
        "pool %d, camera=%d, frame=%dx%d, ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.preview_fps, g_cfg.image_pool_max,
        g_cfg.camera_index, g_cfg.capture_w, g_cfg.capture_h,
        sizeof(ParsedBody));
#endif
}

// ------------------------------------------------------------ ImageDesc ----
// Intrusive ref-counted block: [refcount | ImageDesc | pixels...].
// The ImageDesc sits at offset of member `desc`. The callback receives one
// owned reference and releases it when finished. Cap outstanding blocks and
// drop a frame when the consumer is backlogged.
struct ImgBlock {
    std::atomic<long> ref;
    bool persistent;
    ImageDesc desc;
};

static std::mutex g_imgPoolMtx;
static std::vector<ImgBlock*> g_imgPool;   // free blocks kept for reuse
static std::atomic<long> g_imgAllocated{0}; // all allocated blocks, including the free list
static const size_t kImgPoolKeep = 8;      // free blocks retained

static ImageDesc* make_image_desc(const uint8_t* bgr, int w, int h) {
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    size_t bytes = (size_t)w * h * 3;
    ImgBlock* blk = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_imgPoolMtx);
        for (size_t i = 0; i < g_imgPool.size(); i++) {
            if (static_cast<size_t>(g_imgPool[i]->desc.BytesPerFrame) == bytes) {
                blk = g_imgPool[i];
                g_imgPool.erase(g_imgPool.begin() + (ptrdiff_t)i);
                break;
            }
        }
    }
    if (!blk) {
        long allocated = g_imgAllocated.load(std::memory_order_relaxed);
        do {
            if (allocated >= g_cfg.image_pool_max)
                return nullptr; // callback consumer backlogged: drop this frame
        } while (!g_imgAllocated.compare_exchange_weak(
            allocated, allocated + 1, std::memory_order_acq_rel));
        blk = (ImgBlock*)std::malloc(sizeof(ImgBlock) + bytes);
        if (!blk) {
            g_imgAllocated.fetch_sub(1, std::memory_order_acq_rel);
            return nullptr;
        }
    }
    blk->ref.store(1, std::memory_order_relaxed);
    blk->persistent = false;
    uint8_t* pix = (uint8_t*)(blk + 1);
    if (bgr) std::memcpy(pix, bgr, bytes);
    else std::memset(pix, 0, bytes);
    blk->desc.PointerToFrame = pix;
    blk->desc.Width = w;
    blk->desc.Height = h;
    blk->desc.BitsPerPixel = 24;
    blk->desc.BytesPerPixel = 3;
    blk->desc.BytesPerRow = w * 3;
    blk->desc.BytesPerFrame = (int)bytes;
    return &blk->desc;
}

static ImgBlock* img_block_of(ImageDesc* d) {
    return (ImgBlock*)((char*)d - offsetof(ImgBlock, desc));
}

// Kinect is a live source and the callback dispatcher can discard callbacks
// without releasing their ImageDesc.  Four process-lifetime slots (analysis
// main/sub + preview left/right) keep real camera pixels flowing without
// defeating the bounded ordinary pool. The callback consumer may retain a slot;
// seeing a newer live frame in that case is safe and preferable to a permanent
// black preview after the first few callbacks.
static std::mutex g_reusableImageMtx;
static std::array<ImgBlock*, 4> g_reusableImages {};

static ImageDesc* make_reusable_image_desc(size_t slot, const uint8_t* bgr, int w, int h) {
    if (slot >= g_reusableImages.size()) return nullptr;
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    const size_t bytes = (size_t)w * h * 3;

    std::lock_guard<std::mutex> lk(g_reusableImageMtx);
    ImgBlock* blk = g_reusableImages[slot];
    if (!blk || static_cast<size_t>(blk->desc.BytesPerFrame) != bytes) {
        // Do not free an old differently-sized slot: a callback may still own
        // its pointer. Kinect v1 is fixed at 640x480, so this is only a
        // defensive path for a future backend change.
        blk = (ImgBlock*)std::malloc(sizeof(ImgBlock) + bytes);
        if (!blk) return nullptr;
        blk->persistent = true;
        g_reusableImages[slot] = blk;
        log("kinect image slot %llu allocated (%dx%d, %llu bytes)",
            (unsigned long long)slot, w, h, (unsigned long long)bytes);
    }

    blk->ref.store(1, std::memory_order_relaxed);
    uint8_t* pix = (uint8_t*)(blk + 1);
    if (bgr) std::memcpy(pix, bgr, bytes);
    else std::memset(pix, 0, bytes);
    blk->desc.PointerToFrame = pix;
    blk->desc.Width = w;
    blk->desc.Height = h;
    blk->desc.BitsPerPixel = 24;
    blk->desc.BytesPerPixel = 3;
    blk->desc.BytesPerRow = w * 3;
    blk->desc.BytesPerFrame = (int)bytes;
    return &blk->desc;
}

// ------------------------------------------------------------ timestamp ----
static uint64_t qpc_now_us() {
    static const double freq_inv = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        return 1000000.0 / (double)f.QuadPart;
    }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * freq_inv);
}

// ------------------------------------------------------------ state ----
static std::atomic<bool> g_initialized{false};
static std::shared_ptr<CameraSource> g_cam;
static PoseEngine g_pose;
static std::atomic<bool> g_poseOk{false};

// ------------------------------------------------------------ backend ----
// Release targets select one sensor at compile time. An explicitly selected
// backend never falls through to another physical device on failure.
enum class Backend { Webcam, Kinect, SteamVr, D4xx, D4xxSpike };

static Backend backend_from_env() {
#if defined(ANYGEAR_BACKEND_KINECT)
    return Backend::Kinect;
#elif defined(ANYGEAR_BACKEND_WEBCAM)
    return Backend::Webcam;
#elif defined(ANYGEAR_BACKEND_STEAMVR)
    return Backend::SteamVr;
#elif defined(ANYGEAR_BACKEND_D4XX)
    return Backend::D4xx;
#elif defined(ANYGEAR_BACKEND_D4XX_SPIKE)
    return Backend::D4xxSpike;
#else
    const char* e = std::getenv("VP4U_BACKEND");
    if (!e) return Backend::Webcam;
    std::string v(e);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    if (v == "kinect") return Backend::Kinect;
    if (v == "steamvr") return Backend::SteamVr;
    if (v == "d4xx") return Backend::D4xx;
    if (v == "d4xx-spike") return Backend::D4xxSpike;
    return Backend::Webcam;
#endif
}

static Backend backend() {
    static const Backend b = backend_from_env();
    return b;
}

// Non-null while the corresponding hardware-skeleton backend is running.
static std::shared_ptr<KinectSource> g_kinect;
static std::shared_ptr<SteamVrSource> g_steamvr;
static std::shared_ptr<D4xxSource> g_d4xx;
static std::shared_ptr<SpikeSource> g_spike;

static bool hardware_skeleton_active() {
    return g_kinect != nullptr || g_steamvr != nullptr || g_spike != nullptr;
}

static bool hardware_has_color_stream() {
    return g_kinect && g_kinect->has_color_stream();
}

static bool hardware_wait_for_skeleton(PoseResult* out, uint64_t* generation,
                                       int timeout_ms,
                                       std::int64_t* source_time_ns = nullptr) {
    if (g_kinect) {
        return g_kinect->wait_for_skeleton(out, generation, timeout_ms);
    }
    if (g_steamvr) {
        return g_steamvr->wait_for_skeleton(out, generation, timeout_ms);
    }
    if (g_spike) {
        return g_spike->wait_for_skeleton(
            out, generation, timeout_ms, source_time_ns);
    }
    return false;
}

static const char* hardware_name() {
    return g_kinect ? "Kinect" : (g_steamvr ? "SteamVR"
        : (g_spike ? "D4xx/SPiKE" : "hardware"));
}

static int hardware_body_hold_ms() {
    return g_kinect ? g_cfg.kinect_body_hold_ms
        : (g_spike ? g_cfg.d4xx_body_hold_ms : g_cfg.steamvr_body_hold_ms);
}

// Unified frame-source access for the worker loops.
static int src_native_width() {
    if (g_kinect) return g_kinect->native_width();
    if (g_steamvr) return g_steamvr->native_width();
    if (g_spike) return g_spike->native_width();
    if (g_d4xx) return g_d4xx->native_width();
    return g_cam ? g_cam->native_width() : 0;
}
static int src_native_height() {
    if (g_kinect) return g_kinect->native_height();
    if (g_steamvr) return g_steamvr->native_height();
    if (g_spike) return g_spike->native_height();
    if (g_d4xx) return g_d4xx->native_height();
    return g_cam ? g_cam->native_height() : 0;
}
static void src_get_frame(std::vector<uint8_t>& out, int w, int h) {
    if (g_kinect) g_kinect->get_frame_bgr8(out, w, h);
    else if (g_steamvr) g_steamvr->get_frame_bgr8(out, w, h);
    else if (g_d4xx) g_d4xx->get_frame_bgr8(out, w, h);
    else if (g_cam) g_cam->get_frame_bgr8(out, w, h);
}

struct Worker {
    std::atomic<bool> stop{false};
    std::thread th;
    void join() {
        stop = true;
        if (th.joinable()) th.join();
    }
    ~Worker() { join(); }
};

static Worker g_previewWk, g_analysisWk;

static void sleep_worker_until(Worker& worker,
                               std::chrono::steady_clock::time_point deadline) {
    const auto quantum = std::chrono::milliseconds(4);
    while (!worker.stop) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        const auto remaining = deadline - now;
        std::this_thread::sleep_for(remaining > quantum ? quantum : remaining);
    }
}

static std::string dll_dir_w() {
    static const char module_anchor = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_anchor),
            &module)) {
        return ".";
    }
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return ".";
    }
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return std::string(p.begin(), slash == std::wstring::npos ? p.begin() : p.begin() + (ptrdiff_t)slash);
}

static void paths_default(std::wstring* runtime_dir, std::string* model_path) {
    std::string dir = dll_dir_w();
    const std::string dependency_dir = dir +
        (backend() == Backend::D4xx
            ? "\\dance_around_anygear_d4xx_mediapipe_experimental"
            : "\\dance_around_anygear_webcam");
    *runtime_dir = g_cfg.runtime_dir.empty()
        ? std::wstring(dependency_dir.begin(), dependency_dir.end())
        : std::wstring(g_cfg.runtime_dir.begin(), g_cfg.runtime_dir.end());
    *model_path = g_cfg.model_path.empty()
        ? dependency_dir + "\\pose_landmarker_lite.task"
        : g_cfg.model_path;
}

static std::wstring d4xx_runtime_default() {
    const std::string path = g_cfg.d4xx_runtime_path.empty()
        ? dll_dir_w() +
#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
            "\\dance_around_anygear_d4xx_spike\\realsense2.dll"
#else
            "\\dance_around_anygear_d4xx_mediapipe_experimental\\realsense2.dll"
#endif
        : g_cfg.d4xx_runtime_path;
    return std::wstring(path.begin(), path.end());
}

#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
static std::filesystem::path spike_dependency_directory() {
    return std::filesystem::path(dll_dir_w()) /
        "dance_around_anygear_d4xx_spike";
}

static std::filesystem::path spike_configured_path(
    const std::string& configured,
    const std::filesystem::path& fallback) {
    if (configured.empty()) return fallback;
    std::filesystem::path result(configured);
    return result.is_absolute()
        ? result
        : std::filesystem::path(dll_dir_w()) / result;
}

static SpikeTrackingOptions spike_tracking_options() {
    const std::filesystem::path dependency = spike_dependency_directory();
    SpikeTrackingOptions options;
    options.worker_executable = spike_configured_path(
        g_cfg.spike_worker_executable,
        dependency / "worker" /
            "dance_around_anygear_spike_worker.exe").wstring();
    if (g_cfg.spike_python_module) {
        options.worker_arguments.insert(
            options.worker_arguments.end(), {L"-m", L"anygear_spike.worker"});
    }
    const auto add_path = [&](const wchar_t* name,
                              const std::filesystem::path& path) {
        options.worker_arguments.push_back(name);
        options.worker_arguments.push_back(path.wstring());
    };
    add_path(L"--model", spike_configured_path(
        g_cfg.spike_model_path,
        dependency / "spike-itop-side-primary-fp16.onnx"));
    add_path(L"--runtime-config",
        std::filesystem::path(dll_dir_w()) /
            "dance_around_anygear_d4xx_spike.json");
    add_path(L"--log", spike_configured_path(
        g_cfg.spike_worker_log_path, dependency / "worker.log"));
    options.worker_startup_timeout_ms =
        static_cast<std::uint32_t>(g_cfg.spike_startup_timeout_ms);
    options.face_to_face = g_cfg.d4xx_face_to_face;
    options.auto_center_z = g_cfg.d4xx_auto_center_z;
    options.stage_hip_z = g_cfg.d4xx_stage_hip_z;
    options.mirror_center_x = g_cfg.d4xx_mirror_center_x;
    options.offset_x = g_cfg.d4xx_offset_x;
    options.offset_y = g_cfg.d4xx_offset_y;
    options.offset_z = g_cfg.d4xx_offset_z;
    options.pitch_degrees = g_cfg.d4xx_pitch_degrees;
    options.yaw_degrees = g_cfg.d4xx_yaw_degrees;
    options.roll_degrees = g_cfg.d4xx_roll_degrees;
    options.smoothing = g_cfg.spike_smoothing;
    options.log = [](void*, const char* message) { log("%s", message); };
    return options;
}
#endif

static std::wstring steamvr_runtime_default() {
    const std::string directory = g_cfg.steamvr_runtime_dir.empty()
        ? dll_dir_w() + "\\dance_around_anygear_steamvr"
        : g_cfg.steamvr_runtime_dir;
    return std::wstring(directory.begin(), directory.end());
}

// ------------------------------------------------------- joint mapping ----
static void map_pose_to_body(const PoseResult& pr, ParsedBody& body) {
    body.TrackingState = 2;
    body._padding = 0;
    body.TrackingId = 1;

    auto st_of = [&](int mp) { return pose_tracking_state(pr, mp); };
    auto set = [&](int jt, float x, float y, float z, int st) {
        Joint& j = body.JointData[jt];
        j.TrackingState = st;
        j.PositionX = g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - x) : x;
        j.PositionY = y;
        j.PositionZ = z;
    };
    auto set_mp = [&](int jt, int mp) {
        set(jt, pr.world[mp][0], pr.world[mp][1], pr.world[mp][2], st_of(mp));
    };
    auto set_mid = [&](int jt, int a, int b) {
        set(jt,
            (pr.world[a][0] + pr.world[b][0]) * 0.5f,
            (pr.world[a][1] + pr.world[b][1]) * 0.5f,
            (pr.world[a][2] + pr.world[b][2]) * 0.5f,
            std::min(st_of(a), st_of(b)));
    };
    auto set_blend2 = [&](int jt, int anchor, float anchor_weight,
                          int distal, float distal_weight) {
        set(jt,
            pr.world[anchor][0] * anchor_weight +
                pr.world[distal][0] * distal_weight,
            pr.world[anchor][1] * anchor_weight +
                pr.world[distal][1] * distal_weight,
            pr.world[anchor][2] * anchor_weight +
                pr.world[distal][2] * distal_weight,
            std::min(st_of(anchor), st_of(distal)));
    };
    auto set_blend3 = [&](int jt, int anchor, float anchor_weight,
                          int distal_a, float distal_a_weight,
                          int distal_b, float distal_b_weight) {
        set(jt,
            pr.world[anchor][0] * anchor_weight +
                pr.world[distal_a][0] * distal_a_weight +
                pr.world[distal_b][0] * distal_b_weight,
            pr.world[anchor][1] * anchor_weight +
                pr.world[distal_a][1] * distal_a_weight +
                pr.world[distal_b][1] * distal_b_weight,
            pr.world[anchor][2] * anchor_weight +
                pr.world[distal_a][2] * distal_a_weight +
                pr.world[distal_b][2] * distal_b_weight,
            std::min(st_of(anchor),
                     std::max(st_of(distal_a), st_of(distal_b))));
    };
    // core chain
    set_mid(JT_SpineBase, MP_LeftHip, MP_RightHip);                    // 0
    set_mid(JT_Neck, MP_LeftShoulder, MP_RightShoulder);               // 2
    set_mid(JT_SpineMid, MP_LeftHip, MP_RightShoulder);                // placeholder, refined below
    {
        // SpineMid = midpoint(hip-mid, shoulder-mid)
        float hx = (pr.world[MP_LeftHip][0] + pr.world[MP_RightHip][0]) * 0.5f;
        float hy = (pr.world[MP_LeftHip][1] + pr.world[MP_RightHip][1]) * 0.5f;
        float hz = (pr.world[MP_LeftHip][2] + pr.world[MP_RightHip][2]) * 0.5f;
        float sx = (pr.world[MP_LeftShoulder][0] + pr.world[MP_RightShoulder][0]) * 0.5f;
        float sy = (pr.world[MP_LeftShoulder][1] + pr.world[MP_RightShoulder][1]) * 0.5f;
        float sz = (pr.world[MP_LeftShoulder][2] + pr.world[MP_RightShoulder][2]) * 0.5f;
        set(JT_SpineMid, (hx + sx) * 0.5f, (hy + sy) * 0.5f, (hz + sz) * 0.5f,
            std::min(st_of(MP_LeftHip), st_of(MP_RightShoulder)));
    }
    set_mp(JT_Nose, MP_Nose);                                          // 25
    // Kinect has a real head center represented by the midpoint of our
    // synthesized ears. Keep the legacy nose extrapolation for webcam pose.
    if (hardware_skeleton_active()) {
        set_mid(JT_Head, MP_LeftEar, MP_RightEar);
    } else {
        float nx = pr.world[MP_Nose][0], ny = pr.world[MP_Nose][1], nz = pr.world[MP_Nose][2];
        float kx = body.JointData[JT_Neck].PositionX;
        float ky = body.JointData[JT_Neck].PositionY;
        float kz = body.JointData[JT_Neck].PositionZ;
        // undo mirror for the vector math: work in unmirrored space via neck
        float hx = g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - nx) : nx;
        float hz2 = kz;
        (void)hz2;
        float dx = nx - kx, dy = ny - ky, dz = nz - kz;
        set(JT_Head, hx + dx * 0.6f, ny + dy * 0.6f + 0.05f, nz + dz * 0.6f, st_of(MP_Nose));
    }
    // limbs
    set_mp(JT_ShoulderLeft, MP_LeftShoulder);
    set_mp(JT_ElbowLeft, MP_LeftElbow);
    set_mp(JT_WristLeft, MP_LeftWrist);
    // VP4U's Hand joint is a palm center, while MediaPipe's pinky/index
    // landmarks sit on the hand edge and are unstable in a full-body frame.
    // Keep the palm anchored to the reliable wrist and use both hand edges to
    // retain direction without letting one occluded finger pull the hand onto
    // another limb.
    if (hardware_skeleton_active()) set_mp(JT_HandLeft, MP_LeftPinky);
    else set_blend3(JT_HandLeft, MP_LeftWrist, 0.55f,
                    MP_LeftPinky, 0.225f, MP_LeftIndex, 0.225f);
    set_mp(JT_ShoulderRight, MP_RightShoulder);
    set_mp(JT_ElbowRight, MP_RightElbow);
    set_mp(JT_WristRight, MP_RightWrist);
    if (hardware_skeleton_active()) set_mp(JT_HandRight, MP_RightPinky);
    else set_blend3(JT_HandRight, MP_RightWrist, 0.55f,
                    MP_RightPinky, 0.225f, MP_RightIndex, 0.225f);
    set_mp(JT_HipLeft, MP_LeftHip);
    set_mp(JT_KneeLeft, MP_LeftKnee);
    set_mp(JT_AnkleLeft, MP_LeftAnkle);
    if (hardware_skeleton_active()) set_mp(JT_FootLeft, MP_LeftFootIndex);
    else set_blend3(JT_FootLeft, MP_LeftAnkle, 0.15f,
                    MP_LeftHeel, 0.35f, MP_LeftFootIndex, 0.50f);
    set_mp(JT_HipRight, MP_RightHip);
    set_mp(JT_KneeRight, MP_RightKnee);
    set_mp(JT_AnkleRight, MP_RightAnkle);
    if (hardware_skeleton_active()) set_mp(JT_FootRight, MP_RightFootIndex);
    else set_blend3(JT_FootRight, MP_RightAnkle, 0.15f,
                    MP_RightHeel, 0.35f, MP_RightFootIndex, 0.50f);
    // SpineShoulder = slightly below neck (toward spine-mid)
    {
        const Joint& neck = body.JointData[JT_Neck];
        const Joint& sm = body.JointData[JT_SpineMid];
        set(JT_SpineShoulder,
            (g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - neck.PositionX) : neck.PositionX) * 0.75f + (g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - sm.PositionX) : sm.PositionX) * 0.25f,
            neck.PositionY * 0.75f + sm.PositionY * 0.25f,
            neck.PositionZ * 0.75f + sm.PositionZ * 0.25f,
            std::min(neck.TrackingState, sm.TrackingState));
    }
    if (hardware_skeleton_active()) {
        set_mp(JT_HandTipLeft, MP_LeftIndex);
        set_mp(JT_ThumbLeft, MP_LeftThumb);
        set_mp(JT_HandTipRight, MP_RightIndex);
        set_mp(JT_ThumbRight, MP_RightThumb);
    } else {
        set_blend2(JT_HandTipLeft, MP_LeftWrist, 0.20f,
                   MP_LeftIndex, 0.80f);
        set_blend2(JT_ThumbLeft, MP_LeftWrist, 0.20f,
                   MP_LeftThumb, 0.80f);
        set_blend2(JT_HandTipRight, MP_RightWrist, 0.20f,
                   MP_RightIndex, 0.80f);
        set_blend2(JT_ThumbRight, MP_RightWrist, 0.20f,
                   MP_RightThumb, 0.80f);
    }
    set_mp(JT_EyeLeft, MP_LeftEye);
    set_mp(JT_EyeRight, MP_RightEye);
    set_mp(JT_EarLeft, MP_LeftEar);
    set_mp(JT_EarRight, MP_RightEar);
}

static void make_empty_body(ParsedBody& body, int tracking_state) {
    std::memset(&body, 0, sizeof body);
    body.TrackingState = tracking_state;
    body.TrackingId = 1;
}

// ------------------------------------------------------- preview worker ----
static void preview_loop(OnVideoFrameCallback lcb, void* lctx,
                         OnVideoFrameCallback rcb, void* rctx) {
    log("preview: started (left %p right %p)", (void*)lcb, (void*)rcb);
    std::vector<uint8_t> frame;
    const bool skeleton_only = hardware_skeleton_active() &&
                               !hardware_has_color_stream();
    const int effective_preview_fps = skeleton_only ? 1 : g_cfg.preview_fps;
    const auto interval = std::chrono::microseconds(
        1000000 / std::max(1, effective_preview_fps));
    auto next_tick = std::chrono::steady_clock::now();
    static const uint8_t black_pixel[3] = {0, 0, 0};
    if (skeleton_only) {
        log("preview: VP4U_KINECT_NO_COLOR, publishing shared 1x1 sentinel at 1 fps");
    }
    while (!g_previewWk.stop) {
        int w = skeleton_only ? 1
            : (hardware_skeleton_active() ? src_native_width() : g_cfg.capture_w);
        int h = skeleton_only ? 1
            : (hardware_skeleton_active() ? src_native_height() : g_cfg.capture_h);
        if (w <= 0) w = 640;
        if (h <= 0) h = 480;
        if (!skeleton_only) src_get_frame(frame, w, h);
        const uint8_t* pixels = skeleton_only ? black_pixel
            : (frame.empty() ? nullptr : frame.data());
        uint64_t ts = qpc_now_us();
        if (hardware_skeleton_active()) {
            // A skeleton backend is a single source. Both VP4U "eyes" may share this
            // process-lifetime descriptor; ReleaseImageDesc is intentionally
            // a no-op for reusable slots.
            ImageDesc* shared = make_reusable_image_desc(2, pixels, w, h);
            if (shared && lcb) lcb(lctx, ts, shared);
            if (shared && rcb) rcb(rctx, ts, shared);
        } else {
            if (lcb) {
                ImageDesc* d = make_image_desc(pixels, w, h);
                if (d) lcb(lctx, ts, d);
            }
            if (rcb) {
                ImageDesc* d = make_image_desc(pixels, w, h);
                if (d) rcb(rctx, ts, d);
            }
        }
        next_tick += interval;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick < now) next_tick = now;
        sleep_worker_until(g_previewWk, next_tick);
    }
    log("preview: stopped");
}

// Convert Kinect NUI camera space into the coordinate convention emitted by
// the VP4U-compatible pose path. Keep this at the backend boundary because the
// shared coordinate mapper must not contain Kinect-specific assumptions.
static void transform_kinect_pose_to_stage(PoseResult& pose) {
    for (int i = 0; i < kMpLandmarkCount; ++i) {
        const float camera_x = pose.world[i][0];
        pose.world[i][0] = (g_cfg.kinect_flip_x ? -camera_x : camera_x)
                         + g_cfg.kinect_offset_x;
        pose.world[i][1] += g_cfg.kinect_offset_y;
        pose.world[i][2] += g_cfg.kinect_offset_z;
    }
    if (g_cfg.kinect_face_to_face) {
        // Rotate 180 degrees around a vertical line through the configured
        // horizontal center, while retaining the live hip as the depth pivot.
        // Unlike a hip-local yaw, the fixed X
        // pivot mirrors whole-body translation too: performer-left remains
        // screen-left. The rigid X+Z rotation also swaps the shoulder/hip
        // basis naturally, so no semantic left/right swap is needed.
        const float hip_z = (pose.world[MP_LeftHip][2] +
                             pose.world[MP_RightHip][2]) * 0.5f;
        for (int i = 0; i < kMpLandmarkCount; ++i) {
            pose.world[i][0] = 2.0f * g_cfg.kinect_mirror_center_x
                             - pose.world[i][0];
            pose.world[i][2] = 2.0f * hip_z - pose.world[i][2];
        }
    }
}

struct D4xxInferenceState {
    std::mutex mutex;
    ParsedBody previous{};
    ParsedBody latest{};
    bool have_previous = false;
    bool have_latest = false;
    bool last_attempt_valid = false;
    uint64_t previous_sample_ms = 0;
    uint64_t latest_sample_ms = 0;
    uint64_t last_valid_ms = 0;
    uint64_t attempts = 0;
    uint64_t detections = 0;
};

static void d4xx_analysis_loop(OnPoseFrameCallback cb, void* ctx) {
    D4xxInferenceState state;
    std::thread inference([&state] {
        std::vector<uint8_t> frame;
        auto next_tick = std::chrono::steady_clock::now();
        while (!g_analysisWk.stop) {
            const uint64_t sample_ms = GetTickCount64();
            ParsedBody detected_body;
            make_empty_body(detected_body, 0);
            bool detected = false;
            const int width = g_cfg.capture_w;
            const int height = g_cfg.capture_h;
            if (g_poseOk && g_d4xx &&
                g_d4xx->get_pose_frame_bgr8(frame, width, height) &&
                !frame.empty()) {
                PoseResult pose;
                if (g_pose.infer(frame.data(), width, height, &pose) &&
                    pose.valid && g_d4xx->apply_depth_to_pose(&pose)) {
                    map_pose_to_body(pose, detected_body);
                    detected = true;
                }
            }
            const uint64_t completion_ms = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                ++state.attempts;
                state.last_attempt_valid = detected;
                if (detected) {
                    if (state.have_latest) {
                        state.previous = state.latest;
                        state.previous_sample_ms = state.latest_sample_ms;
                        state.have_previous = true;
                    }
                    state.latest = detected_body;
                    state.latest_sample_ms = sample_ms;
                    state.last_valid_ms = completion_ms;
                    state.have_latest = true;
                    ++state.detections;
                }
            }
            next_tick += std::chrono::microseconds(
                1000000 / std::max(1, g_cfg.capture_fps));
            const auto now = std::chrono::steady_clock::now();
            if (next_tick < now) next_tick = now;
            sleep_worker_until(g_analysisWk, next_tick);
        }
    });

    ParsedBody body;
    make_empty_body(body, 0);
    bool was_tracked = false;
    uint64_t output_frames = 0;
    uint64_t last_attempts = 0;
    uint64_t last_detections = 0;
    uint64_t telemetry_outputs = 0;
    auto telemetry_tick = std::chrono::steady_clock::now();
    auto next_tick = telemetry_tick;
    static const uint8_t black_pixel[3] = {0, 0, 0};
    while (!g_analysisWk.stop) {
        const uint64_t now_ms = GetTickCount64();
        bool have_body = false;
        bool latest_attempt_valid = false;
        uint64_t attempts = 0;
        uint64_t detections = 0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            attempts = state.attempts;
            detections = state.detections;
            latest_attempt_valid = state.last_attempt_valid;
            have_body = state.have_latest && state.last_valid_ms != 0 &&
                now_ms - state.last_valid_ms <=
                    static_cast<uint64_t>(g_cfg.d4xx_body_hold_ms);
            if (have_body) {
                if (state.have_previous) {
                    predict_body(
                        &body, state.previous, state.latest,
                        state.previous_sample_ms, state.latest_sample_ms,
                        now_ms, g_cfg.d4xx_prediction_ms);
                } else {
                    body = state.latest;
                }
                if (!latest_attempt_valid &&
                    now_ms - state.last_valid_ms > 100) {
                    body.TrackingState = 1;
                    for (int joint = 0; joint < kJointCount; ++joint) {
                        if (body.JointData[joint].TrackingState != 0) {
                            body.JointData[joint].TrackingState = 1;
                        }
                    }
                }
            }
        }
        if (!have_body) make_empty_body(body, 0);
        if (have_body != was_tracked) {
            log("analysis: D4xx IR/MediaPipe body -> %s",
                have_body ? "TRACKED" : "NOT TRACKED");
            was_tracked = have_body;
        }

        ImageDesc* main_image = make_reusable_image_desc(
            0, black_pixel, 1, 1);
        cb(ctx, qpc_now_us(), &body, 1,
           nullptr, 0, nullptr, 0, nullptr, 0,
           main_image, nullptr);
        ++output_frames;
        ++telemetry_outputs;

        const auto now = std::chrono::steady_clock::now();
        if (now - telemetry_tick >= std::chrono::seconds(10)) {
            const float seconds = std::chrono::duration<float>(
                now - telemetry_tick).count();
            log("analysis: D4xx cadence infer=%.1fHz tracked=%.1fHz "
                "output=%.1fHz",
                (attempts - last_attempts) / seconds,
                (detections - last_detections) / seconds,
                telemetry_outputs / seconds);
            telemetry_tick = now;
            last_attempts = attempts;
            last_detections = detections;
            telemetry_outputs = 0;
        }

        next_tick += std::chrono::microseconds(
            1000000 / std::max(1, g_cfg.d4xx_output_fps));
        const auto after_callback = std::chrono::steady_clock::now();
        if (next_tick < after_callback) next_tick = after_callback;
        sleep_worker_until(g_analysisWk, next_tick);
    }
    if (inference.joinable()) inference.join();
    log("analysis: D4xx stopped after %llu output frames",
        (unsigned long long)output_frames);
}

// ------------------------------------------------------ analysis worker ----
static void analysis_loop(OnPoseFrameCallback cb, void* ctx) {
    const bool hardware = hardware_skeleton_active();
    log("runtime 5/5: analysis started (%s)", g_kinect ? "kinect NUI skeleton"
        : (g_steamvr ? "SteamVR tracked poses"
        : (g_spike ? "D4xx depth + SPiKE worker"
                     : (g_poseOk ? (g_d4xx
                                       ? "D4xx IR + MediaPipe + metric depth"
                                       : "MediaPipe Pose Landmarker")
                                  : "MediaPipe unavailable (empty frames)"))));
    if (g_d4xx && !hardware) {
        d4xx_analysis_loop(cb, ctx);
        return;
    }
    std::vector<uint8_t> frame;
    ParsedBody body;
    make_empty_body(body, 0);
    int miss = 999; // frames since last valid detection
    uint64_t frame_no = 0;
    uint64_t last_detected_ms = 0;
    uint64_t hardware_generation = 0;
    bool last_hardware_valid = false;
    bool spike_last_worker_valid = false;
    ParsedBody spike_previous_body{};
    ParsedBody spike_latest_body{};
    double spike_previous_sample_ms = 0.0;
    double spike_latest_sample_ms = 0.0;
    bool spike_have_previous = false;
    bool spike_have_latest = false;
    bool last_webcam_valid = false;
    auto next_webcam_tick = std::chrono::steady_clock::now();
    auto next_spike_tick = std::chrono::steady_clock::now();
    static const uint8_t black_pixel[3] = {0, 0, 0};
    while (!g_analysisWk.stop) {
        int w = hardware ? src_native_width() : g_cfg.capture_w;
        int h = hardware ? src_native_height() : g_cfg.capture_h;
        if (w <= 0) w = 640;
        if (h <= 0) h = 480;

        bool detected = false;
        bool hardware_sample_received = false;
        std::int64_t hardware_source_time_ns = 0;
        if (hardware) {
            // SPiKE inference is intentionally slower than the game's pose
            // stream. Poll its latest generation without blocking and submit
            // the retained body at the configured 30 Hz callback cadence.
            // Native skeleton backends remain generation-driven.
            PoseResult pr;
            const bool skeleton_streaming = hardware_wait_for_skeleton(
                &pr, &hardware_generation, g_spike ? 0 : 100,
                &hardware_source_time_ns);
            hardware_sample_received = skeleton_streaming;
            if (g_spike && skeleton_streaming) {
                spike_last_worker_valid = pr.valid;
            }
            if (!skeleton_streaming && !g_spike) {
                if (g_analysisWk.stop) break;
                continue;
            }
            if (skeleton_streaming && pr.valid) {
                if (g_kinect) transform_kinect_pose_to_stage(pr);
                ParsedBody measured_body{};
                map_pose_to_body(pr, measured_body);
                if (g_spike && hardware_source_time_ns > 0) {
                    const double sample_ms =
                        static_cast<double>(hardware_source_time_ns) / 1.0e6;
                    const bool time_advanced = !spike_have_latest ||
                        sample_ms > spike_latest_sample_ms + 0.001;
                    if (time_advanced) {
                        const bool continuous = spike_have_latest &&
                            sample_ms - spike_latest_sample_ms <= 250.0;
                        if (continuous) {
                            spike_previous_body = spike_latest_body;
                            spike_previous_sample_ms = spike_latest_sample_ms;
                            spike_have_previous = true;
                        } else {
                            spike_have_previous = false;
                        }
                        spike_latest_body = measured_body;
                        spike_latest_sample_ms = sample_ms;
                        spike_have_latest = true;
                    } else if (sample_ms < spike_latest_sample_ms - 0.001) {
                        // A rebuilt input source must not inherit old velocity.
                        spike_latest_body = measured_body;
                        spike_latest_sample_ms = sample_ms;
                        spike_have_latest = true;
                        spike_have_previous = false;
                    }
                } else {
                    body = measured_body;
                }
                detected = true;
            }

            if (g_spike && spike_have_latest) {
                body = spike_latest_body;
                if (spike_have_previous) {
                    const double callback_time_ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                    predict_body(
                        &body, spike_previous_body, spike_latest_body,
                        spike_previous_sample_ms, spike_latest_sample_ms,
                        callback_time_ms, g_cfg.d4xx_prediction_ms);
                }
            }

            const bool valid_now = skeleton_streaming && pr.valid;
            if (skeleton_streaming && valid_now != last_hardware_valid) {
                if (valid_now) {
                    const float hip_x = (pr.world[MP_LeftHip][0] +
                                         pr.world[MP_RightHip][0]) * 0.5f;
                    const float hip_y = (pr.world[MP_LeftHip][1] +
                                         pr.world[MP_RightHip][1]) * 0.5f;
                    const float hip_z = (pr.world[MP_LeftHip][2] +
                                         pr.world[MP_RightHip][2]) * 0.5f;
                    log(
                        "analysis: %s body -> TRACKED, "
                        "hip=(%.3f, %.3f, %.3f), confidence=%.2f",
                        hardware_name(), hip_x, hip_y, hip_z, pr.confidence);
                } else if (last_hardware_valid) {
                    log("analysis: %s body -> NOT TRACKED", hardware_name());
                }
                last_hardware_valid = valid_now;
            }
        } else {
            src_get_frame(frame, w, h);
            if (g_poseOk && !frame.empty()) {
                PoseResult pr;
                if (g_pose.infer(frame.data(), w, h, &pr) && pr.valid) {
                    if (g_d4xx) g_d4xx->apply_depth_to_pose(&pr);
                    map_pose_to_body(pr, body);
                    detected = true;
                }
            }
        }
        if (detected) {
            miss = 0;
            last_detected_ms = GetTickCount64();
        } else {
            miss++;
            const uint64_t now_ms = GetTickCount64();
            const bool hardware_grace = hardware && body.TrackingState != 0 &&
                last_detected_ms != 0 &&
                now_ms - last_detected_ms <=
                    (uint64_t)hardware_body_hold_ms();
            const bool d4xx_grace = !hardware && g_d4xx &&
                body.TrackingState != 0 && last_detected_ms != 0 &&
                now_ms - last_detected_ms <=
                    (uint64_t)g_cfg.d4xx_body_hold_ms;
            const bool spike_resample = g_spike &&
                !hardware_sample_received &&
                spike_last_worker_valid &&
                body.TrackingState != 0 && last_detected_ms != 0 &&
                now_ms - last_detected_ms <=
                    (uint64_t)hardware_body_hold_ms();
            if (d4xx_grace || spike_resample) {
                // The IR pose detector can briefly return no result on a valid
                // video track. SPiKE also retains its latest measured pose
                // between model generations. Keep joint states intact for
                // these expected resampling frames.
            } else if (hardware_grace ||
                       (!hardware && miss <= 5 && body.TrackingState != 0)) {
                // Bridge short hardware identity/occlusion gaps with the last stable
                // body, but mark every joint inferred. A real departure still
                // becomes NotTracked after the backend's configured hold time.
                body.TrackingState = 1;
                for (int j = 0; j < kJointCount; ++j) {
                    if (body.JointData[j].TrackingState != 0) {
                        body.JointData[j].TrackingState = 1;
                    }
                }
            } else {
                make_empty_body(body, 0);
                if (g_spike) {
                    spike_have_latest = false;
                    spike_have_previous = false;
                    spike_last_worker_valid = false;
                }
            }
        }
        if (!hardware) {
            const bool valid_now = body.TrackingState != 0;
            if (valid_now != last_webcam_valid) {
                log("analysis: %s body -> %s",
                    g_d4xx ? "D4xx IR/MediaPipe" : "MediaPipe",
                    valid_now ? "TRACKED" : "NOT TRACKED");
                last_webcam_valid = valid_now;
            }
        }

        uint64_t ts = qpc_now_us();
        ImageDesc* main_img = nullptr;
        ImageDesc* sub_img = nullptr;
        const bool compact_pose_images = hardware || g_d4xx;
        if (compact_pose_images) {
            if (g_kinect && g_cfg.kinect_pose_images &&
                g_kinect->has_color_stream()) {
                src_get_frame(frame, w, h);
                const uint8_t* pixels = frame.empty() ? nullptr : frame.data();
                main_img = make_reusable_image_desc(0, pixels, w, h);
                sub_img = make_reusable_image_desc(1, pixels, w, h);
            } else {
                // A persistent 1x1 descriptor is the minimum callback token
                // that carries the real 3D body without RGB traffic.
                main_img = make_reusable_image_desc(0, black_pixel, 1, 1);
            }
        } else {
            const uint8_t* pixels = frame.empty() ? nullptr : frame.data();
            main_img = make_image_desc(pixels, w, h);
            sub_img = make_image_desc(pixels, w, h);
        }
        if (!main_img || (!compact_pose_images && !sub_img)) {
            static int drop_log = 0;
            if (++drop_log % 300 == 1)
                log("analysis: image pool cap reached, delivering frame without images");
        }
        cb(ctx, ts,
           &body, 1,
           nullptr, 0,
           nullptr, 0,
           nullptr, 0,
           main_img, sub_img);
        frame_no++;

        if (g_spike) {
            // Submit immediately after sampling. Sleeping before the callback
            // adds one full output period to every measured pose.
            next_spike_tick += std::chrono::microseconds(
                1000000 / std::max(1, g_cfg.d4xx_output_fps));
            const auto now = std::chrono::steady_clock::now();
            if (next_spike_tick < now) next_spike_tick = now;
            sleep_worker_until(g_analysisWk, next_spike_tick);
        } else if (!hardware) {
            // Webcam inference has no hardware-frame wait, so retain a precise
            // bounded cadence without the old 4 ms rounding oversleep.
            next_webcam_tick += std::chrono::microseconds(
                1000000 / std::max(1, g_cfg.capture_fps));
            const auto now = std::chrono::steady_clock::now();
            if (next_webcam_tick < now) next_webcam_tick = now;
            sleep_worker_until(g_analysisWk, next_webcam_tick);
        }
    }
    log("analysis: stopped after %llu frames", (unsigned long long)frame_no);
}

// ----------------------------------------------------- table functions ----
static uint64_t t_GetCurrentTimestamp(void) { return qpc_now_us(); }
static intptr_t t_RegisterLoggerCallback(OnLogCallback cb, void* ctx) { return logger_add(cb, ctx); }
static void     t_UnregisterLoggerCallback(intptr_t h) { logger_remove(h); }

static void t_AddRefImageDesc(ImageDesc* p) {
    if (!p) return;
    ImgBlock* blk = img_block_of(p);
    if (blk->persistent) return;
    blk->ref.fetch_add(1, std::memory_order_relaxed);
}
static void t_ReleaseImageDesc(ImageDesc* p) {
    if (!p) return;
    ImgBlock* blk = img_block_of(p);
    if (blk->persistent) return;
    if (blk->ref.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        bool pooled = false;
        {
            std::lock_guard<std::mutex> lk(g_imgPoolMtx);
            if (g_imgPool.size() < kImgPoolKeep) {
                g_imgPool.push_back(blk);
                pooled = true;
            }
        }
        if (!pooled) {
            std::free(blk);
            g_imgAllocated.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
}

static intptr_t t_Initialize(void) {
    if (g_cfgPath.empty()) config_load_file(nullptr);
    else config_apply_environment();
    g_initialized = true;
    const char* name = backend() == Backend::Kinect ? "kinect"
        : (backend() == Backend::SteamVr ? "steamvr"
            : (backend() == Backend::D4xx ? "d4xx"
                : (backend() == Backend::D4xxSpike
                    ? "d4xx-spike" : "webcam")));
    log("runtime 3/5: initialized VP4U shim (backend=%s)", name);
    return 1;
}

static void t_Shutdown(void) {
    log("Shutdown: stopping workers");
    g_previewWk.join();
    g_analysisWk.join();
    g_kinect.reset();
    g_steamvr.reset();
    g_spike.reset();
    g_d4xx.reset();
    g_cam.reset();
    if (g_poseOk) { g_pose.shutdown(); g_poseOk = false; }
    g_initialized = false;
    log("Shutdown: done");
}

static void t_LoadConfig(const char* appConfigPath) { config_load_file(appConfigPath); }
static void t_ReloadConfig(void) {
    if (!g_cfgPath.empty()) config_load_file(g_cfgPath.c_str());
    else log("ReloadConfig: no config loaded yet");
}

static bool t_OpenRealSense(void) {
    if (backend() == Backend::Kinect) {
        log("runtime 4/5: opening Kinect v1 sensor");
        KinectTrackingOptions options;
        options.smoothing = g_cfg.kinect_smoothing;
        options.correction = g_cfg.kinect_correction;
        options.prediction = g_cfg.kinect_prediction;
        options.jitter_radius = g_cfg.kinect_jitter_radius;
        options.max_deviation_radius = g_cfg.kinect_max_deviation_radius;
        options.skeleton_lock_ms = g_cfg.kinect_skeleton_lock_ms;
        options.body_hold_ms = g_cfg.kinect_body_hold_ms;
        options.max_joint_speed_mps = g_cfg.kinect_max_joint_speed_mps;
        options.bone_length_tolerance = g_cfg.kinect_bone_length_tolerance;
        options.use_color = g_cfg.kinect_use_color;
        options.color_fps = g_cfg.preview_fps;
        g_kinect = KinectSource::acquire(options);
        if (g_kinect) {
            log("runtime 4/5: Kinect sensor opened (%s+skeleton)",
                g_kinect->has_color_stream() ? "color" : "no-color");
            return true;
        }
        log("runtime 4/5: Kinect sensor FAILED");
        return false;
    }
    if (backend() == Backend::SteamVr) {
        log("runtime 4/5: opening SteamVR tracked-pose runtime");
        SteamVrTrackingOptions options;
        options.runtime_dir = steamvr_runtime_default();
        options.capture_fps = g_cfg.capture_fps;
        options.body_hold_ms = g_cfg.steamvr_body_hold_ms;
        options.stage_hip_x = g_cfg.steamvr_stage_hip_x;
        options.stage_hip_y = g_cfg.steamvr_stage_hip_y;
        options.stage_hip_z = g_cfg.steamvr_stage_hip_z;
        options.auto_center = g_cfg.steamvr_auto_center;
        options.face_to_face = g_cfg.steamvr_face_to_face;
        options.require_waist = g_cfg.steamvr_require_waist;
        options.require_feet = g_cfg.steamvr_require_feet;
        options.log = [](void*, const char* message) { log("%s", message); };
        std::string error;
        g_steamvr = SteamVrSource::acquire(options, &error);
        if (g_steamvr) {
            log("runtime 4/5: SteamVR runtime opened (tracked poses, no RGB)");
            return true;
        }
        log("runtime 4/5: SteamVR runtime FAILED: %s", error.c_str());
        return false;
    }
    if (backend() == Backend::D4xx || backend() == Backend::D4xxSpike) {
        log("runtime 4/5: opening D4xx depth%s devices",
            backend() == Backend::D4xxSpike ? "-only" : "+infrared");
        D4xxOptions options;
        options.runtime_path = d4xx_runtime_default();
        options.width = g_cfg.capture_w;
        options.height = g_cfg.capture_h;
        options.fps = g_cfg.capture_fps;
        options.primary_device = g_cfg.d4xx_primary_device;
        options.primary_serial = g_cfg.d4xx_primary_serial;
        options.required_devices = g_cfg.d4xx_required_devices;
        options.infrared_index = g_cfg.d4xx_infrared_index;
        options.frame_stall_timeout_ms = g_cfg.d4xx_frame_stall_timeout_ms;
        options.reconnect_delay_ms = g_cfg.d4xx_reconnect_delay_ms;
        options.depth_scale_m = g_cfg.d4xx_depth_scale_m;
        options.mirror_center_x = g_cfg.d4xx_mirror_center_x;
        options.face_to_face = g_cfg.d4xx_face_to_face;
        options.auto_center_z = g_cfg.d4xx_auto_center_z;
        options.stage_hip_z = g_cfg.d4xx_stage_hip_z;
        options.offset_x = g_cfg.d4xx_offset_x;
        options.offset_y = g_cfg.d4xx_offset_y;
        options.offset_z = g_cfg.d4xx_offset_z;
        options.pitch_degrees = g_cfg.d4xx_pitch_degrees;
        options.yaw_degrees = g_cfg.d4xx_yaw_degrees;
        options.roll_degrees = g_cfg.d4xx_roll_degrees;
        options.smoothing = g_cfg.d4xx_smoothing;
        if (backend() == Backend::D4xxSpike) {
            options.align_depth_to_infrared = false;
            options.enable_infrared = false;
            options.emitter_enabled_by_device = {1, 1};
            options.emitter_on_off_by_device = {0, 0};
            if (g_cfg.d4xx_emitter_mode == "all-off") {
                options.emitter_enabled_by_device = {0, 0};
            } else if (g_cfg.d4xx_emitter_mode == "first-only") {
                options.emitter_enabled_by_device = {1, 0};
            } else if (g_cfg.d4xx_emitter_mode == "second-only") {
                options.emitter_enabled_by_device = {0, 1};
            } else if (g_cfg.d4xx_emitter_mode == "alternating") {
                options.emitter_on_off_by_device = {1, 1};
            }
            options.visual_preset_by_device = {4, 4};
        }
        options.log = [](void*, const char* message) { log("%s", message); };
        std::string error;
        g_d4xx = D4xxSource::acquire(options, &error);
        const bool ready = g_d4xx && g_d4xx->wait_until_ready(8000);
        if (ready) {
            if (backend() == Backend::D4xxSpike) {
#if defined(ANYGEAR_BACKEND_D4XX_SPIKE)
                SpikeTrackingOptions spike_options =
                    spike_tracking_options();
                g_spike = SpikeSource::acquire(
                    g_d4xx, spike_options, &error);
                if (!g_spike) {
                    log("runtime 4/5: SPiKE worker FAILED: %s",
                        error.c_str());
                    g_d4xx.reset();
                    return false;
                }
                log("runtime 4/5: D4xx/SPiKE ready (%d/%d devices, "
                    "native Z16, no RGB)", g_d4xx->active_device_count(),
                    g_d4xx->device_count());
                return true;
#endif
            }
            log("runtime 4/5: D4xx ready (%d/%d devices, IR->BGR24, "
                "depth->3D)", g_d4xx->active_device_count(),
                g_d4xx->device_count());
            return true;
        }
        log("runtime 4/5: D4xx FAILED: %s",
            error.empty() ? "no depth/infrared frame within 8 seconds"
                          : error.c_str());
        g_d4xx.reset();
        return false;
    }
    int n = count_physical_cameras();
    log("runtime 4/5: %d USB camera(s), opening index %d",
        n, g_cfg.camera_index);
    if (g_cfg.camera_index < 0 || g_cfg.camera_index >= n) {
        log("runtime 4/5: USB camera FAILED (index out of range)");
        return false;
    }
    g_cam = CameraSource::acquire(g_cfg.camera_index);
    const bool ok = g_cam && g_cam->wait_until_ready(3000);
    if (ok) {
        log("runtime 4/5: USB camera opened (%dx%d)",
            g_cam->native_width(), g_cam->native_height());
    } else {
        log("runtime 4/5: USB camera FAILED (no frame within 3 seconds)");
        g_cam.reset();
    }
    return ok;
}

static bool t_RealSenseLeftIsActive(void) {
    return hardware_skeleton_active() || (g_d4xx && g_d4xx->left_active()) ||
        (g_cam && g_cam->is_real_camera());
}
static bool t_RealSenseRightIsActive(void) {
    return hardware_skeleton_active() || (g_d4xx && g_d4xx->right_active()) ||
        (g_cam && g_cam->is_real_camera());
}

static bool t_TryRebootRealSense(bool forceReboot) {
    log("TryRebootRealSense(force=%d): noop ok", (int)forceReboot);
    return true;
}
static bool t_TryRebuildRealSenseFilters(void) {
    log("TryRebuildRealSenseFilters: noop ok");
    return true;
}

static int init_pose_engine(const char* productKey, const char* mode) {
    (void)productKey;
    if (backend() == Backend::Kinect || backend() == Backend::SteamVr ||
        backend() == Backend::D4xxSpike) {
        // The game initializes VisionPose before it calls OpenRealSense, so
        // the hardware source is still null here. Select by the configured
        // backend to avoid creating an unused MediaPipe thread pool.
        log("InitVisionPose%s: skipped (%s backend supplies tracked poses)",
            mode, backend() == Backend::Kinect ? "kinect"
                : (backend() == Backend::SteamVr ? "steamvr" : "d4xx-spike"));
        return 1;
    }
    std::wstring runtime_dir;
    std::string model_path;
    paths_default(&runtime_dir, &model_path);
    PoseCalib calib;
    calib.hip_stage_x = g_cfg.hip_stage_x;
    calib.hip_stage_y = g_cfg.hip_stage_y;
    calib.hip_stage_z = g_cfg.hip_stage_z;
    calib.torso_len_m = g_cfg.torso_len_m;
    calib.z_damping = g_cfg.z_damping;
    // D4xx applies its metric-space filter after depth fusion. Filtering the
    // monocular world landmarks first adds a second delay only on joints whose
    // depth sample is temporarily unavailable.
    calib.ema_alpha = backend() == Backend::D4xx ? 1.0f : g_cfg.ema_alpha;
    std::string err;
    if (g_poseOk) { log("InitVisionPose%s: already initialized", mode); return 1; }
    if (g_pose.init(runtime_dir, model_path, calib, &err)) {
        g_poseOk = true;
        log("InitVisionPose%s: MediaPipe engine ready", mode);
    } else {
        log("InitVisionPose%s: engine FAILED: %s (analysis will emit empty frames)",
            mode, err.c_str());
    }
    // The compatibility API uses 1 to acknowledge initialization. Sensor
    // availability is reported separately by the open and analysis stages.
    return 1;
}

static int t_InitVisionPoseRealtime(const char* productKey) {
    return init_pose_engine(productKey, "Realtime");
}
static int t_InitVisionPoseOffline(const char* productKey) {
    return init_pose_engine(productKey, "Offline");
}

static bool t_StartRealtimeCameraPreview(OnVideoFrameCallback lcb, void* lctx,
                                         OnVideoFrameCallback rcb, void* rctx) {
    g_previewWk.join();
    g_previewWk.stop = false;
    g_previewWk.th = std::thread(preview_loop, lcb, lctx, rcb, rctx);
    return true;
}
static void t_StopRealtimeCameraPreview(void) { g_previewWk.join(); }

static bool t_StartStereoCameraCalibration(OnCalibrationStatusCallback progressCb,
                                           OnVideoFrameCallback previewCb, void* ctx) {
    (void)previewCb;
    log("StartStereoCameraCalibration: skipped (compat layer)");
    if (progressCb) progressCb(ctx, 3, 0, 0, "calibration not required (compat layer)");
    return true;
}
static bool t_CommitStereoCameraCalibration(void) { return true; }
static void t_CloseStereoCameraCalibration(void) {}

static bool t_StartRealtimeAnalysis(OnPoseFrameCallback cb, void* ctx) {
    if (!cb) return false;
    g_analysisWk.join();
    g_analysisWk.stop = false;
    g_analysisWk.th = std::thread(analysis_loop, cb, ctx);
    return true;
}
static void t_StopRealtimeAnalysis(void) { g_analysisWk.join(); }

static bool t_PrepareVideoRecording(const char* path) {
    log("PrepareVideoRecording('%s'): noop ok", path ? path : "");
    return true;
}
static bool t_StartVideoRecording(void) { return true; }
static bool t_FinalizeVideoRecording(void) { return true; }
static bool t_DeleteRecordedVideo(const char* path) { (void)path; return true; }
static bool t_StartOfflineAnalysis(const char* path, OnPoseFrameCallback cb, void* ctx) {
    (void)path; (void)cb; (void)ctx;
    log("StartOfflineAnalysis: noop ok");
    return true;
}
static void t_StopOfflineAnalysis(void) {}

} // namespace vp4u

// ------------------------------------------------------------ exports ----
using namespace vp4u;

extern "C" {

__declspec(dllexport) int vp4uGetVersion() { return kApiVersion; }

__declspec(dllexport) bool vp4uPreboot(const char* prebootConfigJson,
                                       OnLogCallback loggerCb, void* loggerCtx) {
    if (loggerCb) logger_add(loggerCb, loggerCtx);
    log("runtime 1/5: host entered VP4U (config=%s)",
        prebootConfigJson ? prebootConfigJson : "(null)");
    return true;
}

__declspec(dllexport) void vp4uShutdown() {
    log("vp4uShutdown");
    if (g_initialized) t_Shutdown();
}

__declspec(dllexport) bool vp4uGetApiTable(int apiVersion, FunctionPointerTable* table,
                                           OnLogCallback loggerCb, void* loggerCtx) {
    if (loggerCb) logger_add(loggerCb, loggerCtx);
    if (!table || apiVersion != kApiVersion) {
        log("vp4uGetApiTable: rejected (apiVersion=%d table=%p)", apiVersion, (void*)table);
        return false;
    }
    table->GetCurrentTimestamp = t_GetCurrentTimestamp;
    table->RegisterLoggerCallback = t_RegisterLoggerCallback;
    table->UnregisterLoggerCallback = t_UnregisterLoggerCallback;
    table->AddRefImageDesc = t_AddRefImageDesc;
    table->ReleaseImageDesc = t_ReleaseImageDesc;
    table->Initialize = t_Initialize;
    table->Shutdown = t_Shutdown;
    table->LoadConfig = t_LoadConfig;
    table->ReloadConfig = t_ReloadConfig;
    table->OpenRealSense = t_OpenRealSense;
    table->RealSenseLeftIsActive = t_RealSenseLeftIsActive;
    table->RealSenseRightIsActive = t_RealSenseRightIsActive;
    table->TryRebootRealSense = t_TryRebootRealSense;
    table->TryRebuildRealSenseFilters = t_TryRebuildRealSenseFilters;
    table->InitVisionPoseRealtime = t_InitVisionPoseRealtime;
    table->InitVisionPoseOffline = t_InitVisionPoseOffline;
    table->StartRealtimeCameraPreview = t_StartRealtimeCameraPreview;
    table->StopRealtimeCameraPreview = t_StopRealtimeCameraPreview;
    table->StartStereoCameraCalibration = t_StartStereoCameraCalibration;
    table->CommitStereoCameraCalibration = t_CommitStereoCameraCalibration;
    table->CloseStereoCameraCalibration = t_CloseStereoCameraCalibration;
    table->StartRealtimeAnalysis = t_StartRealtimeAnalysis;
    table->StopRealtimeAnalysis = t_StopRealtimeAnalysis;
    table->PrepareVideoRecording = t_PrepareVideoRecording;
    table->StartVideoRecording = t_StartVideoRecording;
    table->FinalizeVideoRecording = t_FinalizeVideoRecording;
    table->DeleteRecordedVideo = t_DeleteRecordedVideo;
    table->StartOfflineAnalysis = t_StartOfflineAnalysis;
    table->StopOfflineAnalysis = t_StopOfflineAnalysis;
    log("runtime 2/5: handed 29-slot VP4U table to host (api=%d)", apiVersion);
    return true;
}

} // extern "C"
