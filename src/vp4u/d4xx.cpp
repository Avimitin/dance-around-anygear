// Intel RealSense D4xx depth/infrared capture.
// librealsense is loaded from a plugin-relative path; no import library is used.
#include "d4xx.h"

#include "d4xx/ir_bgr.h"
#include "webcam.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <librealsense2/rs.h>
#include <librealsense2/h/rs_config.h>
#include <librealsense2/h/rs_pipeline.h>
#include <librealsense2/h/rs_processing.h>

namespace vp4u {

namespace {

template <typename T>
T load_function(HMODULE module, const char* name) {
    const FARPROC raw = GetProcAddress(module, name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

struct RealSenseApi {
    HMODULE module = nullptr;

#define ANYGEAR_RS2_FUNCTION(name) decltype(&name) name = nullptr
    ANYGEAR_RS2_FUNCTION(rs2_get_api_version);
    ANYGEAR_RS2_FUNCTION(rs2_get_error_message);
    ANYGEAR_RS2_FUNCTION(rs2_free_error);
    ANYGEAR_RS2_FUNCTION(rs2_create_context);
    ANYGEAR_RS2_FUNCTION(rs2_delete_context);
    ANYGEAR_RS2_FUNCTION(rs2_query_devices);
    ANYGEAR_RS2_FUNCTION(rs2_delete_device_list);
    ANYGEAR_RS2_FUNCTION(rs2_get_device_count);
    ANYGEAR_RS2_FUNCTION(rs2_create_device);
    ANYGEAR_RS2_FUNCTION(rs2_delete_device);
    ANYGEAR_RS2_FUNCTION(rs2_supports_device_info);
    ANYGEAR_RS2_FUNCTION(rs2_get_device_info);
    ANYGEAR_RS2_FUNCTION(rs2_query_sensors);
    ANYGEAR_RS2_FUNCTION(rs2_delete_sensor_list);
    ANYGEAR_RS2_FUNCTION(rs2_get_sensors_count);
    ANYGEAR_RS2_FUNCTION(rs2_create_sensor);
    ANYGEAR_RS2_FUNCTION(rs2_delete_sensor);
    ANYGEAR_RS2_FUNCTION(rs2_is_sensor_extendable_to);
    ANYGEAR_RS2_FUNCTION(rs2_get_depth_scale);
    ANYGEAR_RS2_FUNCTION(rs2_supports_option);
    ANYGEAR_RS2_FUNCTION(rs2_get_option);
    ANYGEAR_RS2_FUNCTION(rs2_set_option);
    ANYGEAR_RS2_FUNCTION(rs2_create_pipeline);
    ANYGEAR_RS2_FUNCTION(rs2_delete_pipeline);
    ANYGEAR_RS2_FUNCTION(rs2_pipeline_stop);
    ANYGEAR_RS2_FUNCTION(rs2_pipeline_poll_for_frames);
    ANYGEAR_RS2_FUNCTION(rs2_pipeline_start_with_config);
    ANYGEAR_RS2_FUNCTION(rs2_delete_pipeline_profile);
    ANYGEAR_RS2_FUNCTION(rs2_create_frame_queue);
    ANYGEAR_RS2_FUNCTION(rs2_delete_frame_queue);
    ANYGEAR_RS2_FUNCTION(rs2_poll_for_frame);
    ANYGEAR_RS2_FUNCTION(rs2_frame_add_ref);
    ANYGEAR_RS2_FUNCTION(rs2_create_align);
    ANYGEAR_RS2_FUNCTION(rs2_start_processing_queue);
    ANYGEAR_RS2_FUNCTION(rs2_process_frame);
    ANYGEAR_RS2_FUNCTION(rs2_delete_processing_block);
    ANYGEAR_RS2_FUNCTION(rs2_create_config);
    ANYGEAR_RS2_FUNCTION(rs2_delete_config);
    ANYGEAR_RS2_FUNCTION(rs2_config_enable_device);
    ANYGEAR_RS2_FUNCTION(rs2_config_enable_stream);
    ANYGEAR_RS2_FUNCTION(rs2_embedded_frames_count);
    ANYGEAR_RS2_FUNCTION(rs2_extract_frame);
    ANYGEAR_RS2_FUNCTION(rs2_release_frame);
    ANYGEAR_RS2_FUNCTION(rs2_get_frame_stream_profile);
    ANYGEAR_RS2_FUNCTION(rs2_get_stream_profile_data);
    ANYGEAR_RS2_FUNCTION(rs2_get_video_stream_intrinsics);
    ANYGEAR_RS2_FUNCTION(rs2_get_frame_data);
    ANYGEAR_RS2_FUNCTION(rs2_get_frame_data_size);
    ANYGEAR_RS2_FUNCTION(rs2_get_frame_width);
    ANYGEAR_RS2_FUNCTION(rs2_get_frame_height);
    ANYGEAR_RS2_FUNCTION(rs2_get_frame_stride_in_bytes);
#undef ANYGEAR_RS2_FUNCTION

    bool load(const std::wstring& path, std::string* error) {
        module = LoadLibraryExW(
            path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) {
            if (error) {
                *error = "LoadLibrary realsense2.dll failed (Win32 " +
                    std::to_string(GetLastError()) + ")";
            }
            return false;
        }

#define ANYGEAR_LOAD_RS2(name) name = load_function<decltype(name)>(module, #name)
        ANYGEAR_LOAD_RS2(rs2_get_api_version);
        ANYGEAR_LOAD_RS2(rs2_get_error_message);
        ANYGEAR_LOAD_RS2(rs2_free_error);
        ANYGEAR_LOAD_RS2(rs2_create_context);
        ANYGEAR_LOAD_RS2(rs2_delete_context);
        ANYGEAR_LOAD_RS2(rs2_query_devices);
        ANYGEAR_LOAD_RS2(rs2_delete_device_list);
        ANYGEAR_LOAD_RS2(rs2_get_device_count);
        ANYGEAR_LOAD_RS2(rs2_create_device);
        ANYGEAR_LOAD_RS2(rs2_delete_device);
        ANYGEAR_LOAD_RS2(rs2_supports_device_info);
        ANYGEAR_LOAD_RS2(rs2_get_device_info);
        ANYGEAR_LOAD_RS2(rs2_query_sensors);
        ANYGEAR_LOAD_RS2(rs2_delete_sensor_list);
        ANYGEAR_LOAD_RS2(rs2_get_sensors_count);
        ANYGEAR_LOAD_RS2(rs2_create_sensor);
        ANYGEAR_LOAD_RS2(rs2_delete_sensor);
        ANYGEAR_LOAD_RS2(rs2_is_sensor_extendable_to);
        ANYGEAR_LOAD_RS2(rs2_get_depth_scale);
        ANYGEAR_LOAD_RS2(rs2_supports_option);
        ANYGEAR_LOAD_RS2(rs2_get_option);
        ANYGEAR_LOAD_RS2(rs2_set_option);
        ANYGEAR_LOAD_RS2(rs2_create_pipeline);
        ANYGEAR_LOAD_RS2(rs2_delete_pipeline);
        ANYGEAR_LOAD_RS2(rs2_pipeline_stop);
        ANYGEAR_LOAD_RS2(rs2_pipeline_poll_for_frames);
        ANYGEAR_LOAD_RS2(rs2_pipeline_start_with_config);
        ANYGEAR_LOAD_RS2(rs2_delete_pipeline_profile);
        ANYGEAR_LOAD_RS2(rs2_create_frame_queue);
        ANYGEAR_LOAD_RS2(rs2_delete_frame_queue);
        ANYGEAR_LOAD_RS2(rs2_poll_for_frame);
        ANYGEAR_LOAD_RS2(rs2_frame_add_ref);
        ANYGEAR_LOAD_RS2(rs2_create_align);
        ANYGEAR_LOAD_RS2(rs2_start_processing_queue);
        ANYGEAR_LOAD_RS2(rs2_process_frame);
        ANYGEAR_LOAD_RS2(rs2_delete_processing_block);
        ANYGEAR_LOAD_RS2(rs2_create_config);
        ANYGEAR_LOAD_RS2(rs2_delete_config);
        ANYGEAR_LOAD_RS2(rs2_config_enable_device);
        ANYGEAR_LOAD_RS2(rs2_config_enable_stream);
        ANYGEAR_LOAD_RS2(rs2_embedded_frames_count);
        ANYGEAR_LOAD_RS2(rs2_extract_frame);
        ANYGEAR_LOAD_RS2(rs2_release_frame);
        ANYGEAR_LOAD_RS2(rs2_get_frame_stream_profile);
        ANYGEAR_LOAD_RS2(rs2_get_stream_profile_data);
        ANYGEAR_LOAD_RS2(rs2_get_video_stream_intrinsics);
        ANYGEAR_LOAD_RS2(rs2_get_frame_data);
        ANYGEAR_LOAD_RS2(rs2_get_frame_data_size);
        ANYGEAR_LOAD_RS2(rs2_get_frame_width);
        ANYGEAR_LOAD_RS2(rs2_get_frame_height);
        ANYGEAR_LOAD_RS2(rs2_get_frame_stride_in_bytes);
#undef ANYGEAR_LOAD_RS2

        const bool complete = rs2_get_api_version && rs2_get_error_message &&
            rs2_free_error && rs2_create_context && rs2_delete_context &&
            rs2_query_devices && rs2_delete_device_list &&
            rs2_get_device_count && rs2_create_device && rs2_delete_device &&
            rs2_supports_device_info && rs2_get_device_info &&
            rs2_query_sensors && rs2_delete_sensor_list &&
            rs2_get_sensors_count && rs2_create_sensor &&
            rs2_delete_sensor && rs2_is_sensor_extendable_to &&
            rs2_get_depth_scale && rs2_supports_option &&
            rs2_get_option && rs2_set_option &&
            rs2_create_pipeline && rs2_delete_pipeline && rs2_pipeline_stop &&
            rs2_pipeline_poll_for_frames && rs2_pipeline_start_with_config &&
            rs2_delete_pipeline_profile && rs2_create_frame_queue &&
            rs2_delete_frame_queue && rs2_poll_for_frame &&
            rs2_frame_add_ref && rs2_create_align &&
            rs2_start_processing_queue && rs2_process_frame &&
            rs2_delete_processing_block && rs2_create_config &&
            rs2_delete_config && rs2_config_enable_device &&
            rs2_config_enable_stream && rs2_embedded_frames_count &&
            rs2_extract_frame && rs2_release_frame &&
            rs2_get_frame_stream_profile && rs2_get_stream_profile_data &&
            rs2_get_video_stream_intrinsics && rs2_get_frame_data &&
            rs2_get_frame_data_size && rs2_get_frame_width &&
            rs2_get_frame_height && rs2_get_frame_stride_in_bytes;
        if (!complete) {
            if (error) *error = "realsense2.dll is missing a required C API export";
            unload();
            return false;
        }
        return true;
    }

    std::string take_error(rs2_error*& value) const {
        if (!value) return {};
        const char* text = rs2_get_error_message(value);
        std::string result = text ? text : "unknown librealsense error";
        rs2_free_error(value);
        value = nullptr;
        return result;
    }

    void unload() {
        if (module) FreeLibrary(module);
        *this = RealSenseApi{};
    }
};

float median_depth(const std::vector<std::uint16_t>& depth, int width,
                   int height, float x, float y, float scale) {
    const int center_x = static_cast<int>(std::lround(x));
    const int center_y = static_cast<int>(std::lround(y));
    std::array<std::uint16_t, 49> samples{};
    std::size_t count = 0;
    for (int dy = -3; dy <= 3; ++dy) {
        const int py = center_y + dy;
        if (py < 0 || py >= height) continue;
        for (int dx = -3; dx <= 3; ++dx) {
            const int px = center_x + dx;
            if (px < 0 || px >= width) continue;
            const std::uint16_t value =
                depth[static_cast<std::size_t>(py) * width + px];
            if (value != 0) samples[count++] = value;
        }
    }
    if (count < 3) return 0.0f;
    auto middle = samples.begin() + static_cast<std::ptrdiff_t>(count / 2);
    std::nth_element(samples.begin(), middle,
                     samples.begin() + static_cast<std::ptrdiff_t>(count));
    const float meters = static_cast<float>(*middle) * scale;
    return meters >= 0.20f && meters <= 8.0f ? meters : 0.0f;
}

} // namespace

struct D4xxSource::Impl {
    struct DeviceStream {
        int configuration_index = -1;
        bool primary = false;
        std::string name;
        std::string serial;
        float depth_scale_m = 0.001f;
        rs2_pipeline* pipeline = nullptr;
        rs2_config* config = nullptr;
        rs2_pipeline_profile* profile = nullptr;
        rs2_processing_block* align = nullptr;
        rs2_frame_queue* align_queue = nullptr;
        std::atomic<bool> active{false};
        std::chrono::steady_clock::time_point last_complete_frame;
        std::chrono::steady_clock::time_point next_reconnect;
        unsigned int reconnect_attempts = 0;
        mutable std::mutex frame_mutex;
        std::vector<std::uint8_t> infrared;
        std::vector<std::uint16_t> depth;
        rs2_intrinsics intrinsics{};
        std::uint64_t sequence = 0;
        std::int64_t host_time_ns = 0;
        bool have_intrinsics = false;
        std::atomic<bool> have_infrared{false};
        std::atomic<bool> have_depth{false};
        std::atomic<int> width{0};
        std::atomic<int> height{0};
    };

    D4xxOptions options;
    RealSenseApi api;
    rs2_context* context = nullptr;
    std::vector<std::unique_ptr<DeviceStream>> devices;
    std::atomic<bool> stop{false};
    std::thread worker;
    mutable std::mutex pose_mutex;
    std::vector<std::uint16_t> pending_pose_depth;
    float pending_pose_depth_scale_m = 0.001f;
    rs2_intrinsics pending_pose_intrinsics{};
    int pending_pose_width = 0;
    int pending_pose_height = 0;
    int pending_pose_pixel_width = 0;
    int pending_pose_pixel_height = 0;
    bool have_pending_pose_depth = false;
    bool have_previous_pose = false;
    float previous_pose[kMpLandmarkCount][3]{};
    bool have_depth_anchor = false;
    float depth_anchor_offset_z = 0.0f;
    std::vector<float> depth_anchor_samples;

    explicit Impl(const D4xxOptions& value) : options(value) {}

    void log(const char* format, ...) const {
        char message[768];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof message, format, args);
        va_end(args);
        if (options.log) options.log(options.log_context, message);
        else {
            std::fprintf(stderr, "[anygear-d4xx] %s\n", message);
            std::fflush(stderr);
        }
    }

    bool check(rs2_error*& error, const char* operation,
               std::string* output = nullptr) {
        if (!error) return true;
        const std::string detail = api.take_error(error);
        if (output) *output = std::string(operation) + ": " + detail;
        return false;
    }

    std::string device_info(rs2_device* device, rs2_camera_info info) {
        rs2_error* error = nullptr;
        if (!api.rs2_supports_device_info(device, info, &error) ||
            !check(error, "rs2_supports_device_info")) {
            return {};
        }
        const char* value = api.rs2_get_device_info(device, info, &error);
        if (!check(error, "rs2_get_device_info") || !value) return {};
        return value;
    }

    int configured_value(const std::vector<int>& values, int index) const {
        return index >= 0 && index < static_cast<int>(values.size())
            ? values[static_cast<std::size_t>(index)]
            : -1;
    }

    bool set_sensor_option(rs2_sensor* sensor, rs2_option option,
                           int requested, const char* name, int device_index,
                           std::string* output) {
        if (requested < 0) return true;
        rs2_error* error = nullptr;
        const auto* sensor_options =
            reinterpret_cast<const rs2_options*>(sensor);
        const int supported = api.rs2_supports_option(
            sensor_options, option, &error);
        if (!check(error, "rs2_supports_option", output)) return false;
        if (!supported) {
            if (output) {
                *output = "device " + std::to_string(device_index) +
                    " does not support " + name;
            }
            return false;
        }
        api.rs2_set_option(sensor_options, option,
                           static_cast<float>(requested), &error);
        if (!check(error, "rs2_set_option", output)) return false;
        const float actual = api.rs2_get_option(sensor_options, option, &error);
        if (!check(error, "rs2_get_option", output)) return false;
        log("device %d %s requested=%d actual=%.0f", device_index, name,
            requested, actual);
        return true;
    }

    void log_sensor_option(rs2_sensor* sensor, rs2_option option,
                           const char* name, int device_index) {
        rs2_error* error = nullptr;
        const auto* sensor_options =
            reinterpret_cast<const rs2_options*>(sensor);
        const int supported = api.rs2_supports_option(
            sensor_options, option, &error);
        if (error) {
            log("device %d query %s: %s", device_index, name,
                api.take_error(error).c_str());
            return;
        }
        if (!supported) return;
        const float value = api.rs2_get_option(sensor_options, option, &error);
        if (error) {
            log("device %d read %s: %s", device_index, name,
                api.take_error(error).c_str());
            return;
        }
        log("device %d %s=%.3f", device_index, name, value);
    }

    float configure_device(rs2_device* device, int device_index,
                           std::string* output) {
        rs2_error* error = nullptr;
        rs2_sensor_list* sensors = api.rs2_query_sensors(device, &error);
        if (!check(error, "rs2_query_sensors", output) || !sensors) {
            return options.depth_scale_m;
        }
        const int count = api.rs2_get_sensors_count(sensors, &error);
        if (!check(error, "rs2_get_sensors_count", output)) {
            api.rs2_delete_sensor_list(sensors);
            return options.depth_scale_m;
        }
        float result = options.depth_scale_m;
        bool configured = true;
        for (int index = 0; index < count; ++index) {
            rs2_sensor* sensor = api.rs2_create_sensor(sensors, index, &error);
            if (!check(error, "rs2_create_sensor", output) || !sensor) {
                configured = false;
                break;
            }
            const int is_depth = api.rs2_is_sensor_extendable_to(
                sensor, RS2_EXTENSION_DEPTH_SENSOR, &error);
            if (!check(error, "rs2_is_sensor_extendable_to", output)) {
                configured = false;
                api.rs2_delete_sensor(sensor);
                break;
            }
            if (is_depth) {
                const float scale = api.rs2_get_depth_scale(sensor, &error);
                if (check(error, "rs2_get_depth_scale", output) &&
                    scale > 0.0f) {
                    result = scale;
                } else {
                    configured = false;
                }
                if (configured) {
                    configured = set_sensor_option(
                        sensor, RS2_OPTION_VISUAL_PRESET,
                        configured_value(options.visual_preset_by_device,
                                         device_index),
                        "visual-preset", device_index, output);
                }
                // Disable any prior alternating state before selecting a
                // fixed emitter state. The explicit alternating mode sets the
                // enabled state first, then turns alternation back on.
                const int emitter = configured_value(
                    options.emitter_enabled_by_device, device_index);
                const int alternating = configured_value(
                    options.emitter_on_off_by_device, device_index);
                if (configured && emitter >= 0 && alternating != 1) {
                    configured = set_sensor_option(
                        sensor, RS2_OPTION_EMITTER_ON_OFF, 0,
                        "emitter-on-off", device_index, output);
                }
                if (configured) {
                    configured = set_sensor_option(
                        sensor, RS2_OPTION_EMITTER_ENABLED, emitter,
                        "emitter-enabled", device_index, output);
                }
                if (configured) {
                    configured = set_sensor_option(
                        sensor, RS2_OPTION_EMITTER_ON_OFF, alternating,
                        "emitter-on-off", device_index, output);
                }
                if (configured) {
                    log_sensor_option(sensor, RS2_OPTION_VISUAL_PRESET,
                                      "visual-preset", device_index);
                    log_sensor_option(sensor, RS2_OPTION_LASER_POWER,
                                      "laser-power", device_index);
                    log_sensor_option(sensor, RS2_OPTION_EMITTER_ENABLED,
                                      "emitter-enabled", device_index);
                    log_sensor_option(sensor, RS2_OPTION_EMITTER_ON_OFF,
                                      "emitter-on-off", device_index);
                    log_sensor_option(sensor, RS2_OPTION_INTER_CAM_SYNC_MODE,
                                      "inter-cam-sync", device_index);
                    log_sensor_option(sensor, RS2_OPTION_EXPOSURE,
                                      "exposure", device_index);
                    log_sensor_option(sensor, RS2_OPTION_GAIN,
                                      "gain", device_index);
                }
                api.rs2_delete_sensor(sensor);
                break;
            }
            api.rs2_delete_sensor(sensor);
        }
        api.rs2_delete_sensor_list(sensors);
        if (!configured && output && output->empty()) {
            *output = "could not configure depth sensor for device " +
                std::to_string(device_index);
        }
        return result;
    }

    void invalidate_device_frames(DeviceStream& device) {
        device.active.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(device.frame_mutex);
            device.infrared.clear();
            device.depth.clear();
            device.host_time_ns = 0;
            device.have_intrinsics = false;
            device.have_infrared.store(false, std::memory_order_release);
            device.have_depth.store(false, std::memory_order_release);
        }
        if (device.primary) {
            std::lock_guard<std::mutex> lock(pose_mutex);
            pending_pose_depth.clear();
            have_pending_pose_depth = false;
        }
    }

    void cleanup_device(DeviceStream& device) {
        invalidate_device_frames(device);
        rs2_error* error = nullptr;
        if (device.profile && device.pipeline) {
            api.rs2_pipeline_stop(device.pipeline, &error);
            if (error) {
                log("stop %s: %s", device.serial.c_str(),
                    api.take_error(error).c_str());
            }
        }
        if (device.align) api.rs2_delete_processing_block(device.align);
        if (device.align_queue) api.rs2_delete_frame_queue(device.align_queue);
        if (device.profile) api.rs2_delete_pipeline_profile(device.profile);
        if (device.config) api.rs2_delete_config(device.config);
        if (device.pipeline) api.rs2_delete_pipeline(device.pipeline);
        device.profile = nullptr;
        device.align = nullptr;
        device.align_queue = nullptr;
        device.config = nullptr;
        device.pipeline = nullptr;
    }

    bool start_device(DeviceStream& stream, std::string* output) {
        rs2_error* error = nullptr;
        stream.pipeline = api.rs2_create_pipeline(context, &error);
        if (!check(error, "rs2_create_pipeline", output) || !stream.pipeline) {
            return false;
        }
        stream.config = api.rs2_create_config(&error);
        if (!check(error, "rs2_create_config", output) || !stream.config) {
            cleanup_device(stream);
            return false;
        }
        api.rs2_config_enable_device(stream.config, stream.serial.c_str(), &error);
        if (!check(error, "rs2_config_enable_device", output)) {
            cleanup_device(stream);
            return false;
        }
        api.rs2_config_enable_stream(
            stream.config, RS2_STREAM_DEPTH, 0, options.width, options.height,
            RS2_FORMAT_Z16, options.fps, &error);
        if (!check(error, "enable depth stream", output)) {
            cleanup_device(stream);
            return false;
        }
        if (options.enable_infrared) {
            api.rs2_config_enable_stream(
                stream.config, RS2_STREAM_INFRARED, options.infrared_index,
                options.width, options.height, RS2_FORMAT_Y8, options.fps,
                &error);
            if (!check(error, "enable infrared stream", output)) {
                cleanup_device(stream);
                return false;
            }
        }
        stream.profile = api.rs2_pipeline_start_with_config(
            stream.pipeline, stream.config, &error);
        if (!check(error, "rs2_pipeline_start_with_config", output) ||
            !stream.profile) {
            cleanup_device(stream);
            return false;
        }
        if (!options.align_depth_to_infrared) {
            stream.last_complete_frame = std::chrono::steady_clock::now();
            stream.active.store(true, std::memory_order_release);
            return true;
        }
        stream.align_queue = api.rs2_create_frame_queue(1, &error);
        if (!check(error, "rs2_create_frame_queue", output) ||
            !stream.align_queue) {
            cleanup_device(stream);
            return false;
        }
        stream.align = api.rs2_create_align(RS2_STREAM_INFRARED, &error);
        if (!check(error, "rs2_create_align(infrared)", output) ||
            !stream.align) {
            cleanup_device(stream);
            return false;
        }
        api.rs2_start_processing_queue(
            stream.align, stream.align_queue, &error);
        if (!check(error, "rs2_start_processing_queue", output)) {
            cleanup_device(stream);
            return false;
        }
        stream.last_complete_frame = std::chrono::steady_clock::now();
        stream.active.store(true, std::memory_order_release);
        return true;
    }

    bool configure_connected_device(DeviceStream& stream,
                                    std::string* output) {
        rs2_error* error = nullptr;
        rs2_device_list* list = api.rs2_query_devices(context, &error);
        if (!check(error, "rs2_query_devices(reconnect)", output) || !list) {
            return false;
        }
        const int count = api.rs2_get_device_count(list, &error);
        if (!check(error, "rs2_get_device_count(reconnect)", output)) {
            api.rs2_delete_device_list(list);
            return false;
        }
        bool found = false;
        for (int index = 0; index < count; ++index) {
            rs2_device* device = api.rs2_create_device(list, index, &error);
            if (!check(error, "rs2_create_device(reconnect)", output) ||
                !device) {
                break;
            }
            const std::string serial = device_info(
                device, RS2_CAMERA_INFO_SERIAL_NUMBER);
            if (serial == stream.serial) {
                std::string configure_error;
                stream.depth_scale_m = configure_device(
                    device, stream.configuration_index, &configure_error);
                api.rs2_delete_device(device);
                if (!configure_error.empty()) {
                    if (output) *output = configure_error;
                    api.rs2_delete_device_list(list);
                    return false;
                }
                found = true;
                break;
            }
            api.rs2_delete_device(device);
        }
        api.rs2_delete_device_list(list);
        if (!found && output && output->empty()) {
            *output = "serial " + stream.serial + " is not connected";
        }
        return found;
    }

    void make_device_unavailable(DeviceStream& stream,
                                 const std::string& reason) {
        cleanup_device(stream);
        ++stream.reconnect_attempts;
        stream.next_reconnect = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(options.reconnect_delay_ms);
        log("device %s unavailable: %s; reconnect in %d ms",
            stream.serial.c_str(), reason.c_str(),
            options.reconnect_delay_ms);
    }

    bool reconnect_device(DeviceStream& stream) {
        std::string error;
        if (configure_connected_device(stream, &error) &&
            start_device(stream, &error)) {
            log("device %s pipeline restarted on attempt %u",
                stream.serial.c_str(), stream.reconnect_attempts);
            return true;
        }
        ++stream.reconnect_attempts;
        stream.next_reconnect = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(options.reconnect_delay_ms);
        log("device %s reconnect attempt %u failed: %s; retry in %d ms",
            stream.serial.c_str(), stream.reconnect_attempts,
            error.empty() ? "unknown librealsense error" : error.c_str(),
            options.reconnect_delay_ms);
        return false;
    }

    bool open(std::string* output) {
        options.frame_stall_timeout_ms = std::clamp(
            options.frame_stall_timeout_ms, 100, 60000);
        options.reconnect_delay_ms = std::clamp(
            options.reconnect_delay_ms, 50, 60000);
        if (options.align_depth_to_infrared && !options.enable_infrared) {
            if (output) {
                *output = "depth-to-infrared alignment requires the "
                    "infrared stream";
            }
            return false;
        }
        if (!api.load(options.runtime_path, output)) return false;

        rs2_error* error = nullptr;
        const int api_version = api.rs2_get_api_version(&error);
        if (!check(error, "rs2_get_api_version", output)) return false;
        context = api.rs2_create_context(api_version, &error);
        if (!check(error, "rs2_create_context", output) || !context) {
            return false;
        }
        rs2_device_list* list = api.rs2_query_devices(context, &error);
        if (!check(error, "rs2_query_devices", output) || !list) return false;
        const int count = api.rs2_get_device_count(list, &error);
        if (!check(error, "rs2_get_device_count", output)) {
            api.rs2_delete_device_list(list);
            return false;
        }
        log("librealsense API %d; discovered %d device(s)", api_version, count);

        for (int index = 0; index < count; ++index) {
            rs2_device* device = api.rs2_create_device(list, index, &error);
            if (!check(error, "rs2_create_device", output) || !device) break;
            auto stream = std::make_unique<DeviceStream>();
            stream->configuration_index = index;
            stream->name = device_info(device, RS2_CAMERA_INFO_NAME);
            stream->serial = device_info(device, RS2_CAMERA_INFO_SERIAL_NUMBER);
            std::string configure_error;
            stream->depth_scale_m = configure_device(
                device, index, &configure_error);
            api.rs2_delete_device(device);
            if (!configure_error.empty()) {
                if (output) *output = configure_error;
                api.rs2_delete_device_list(list);
                return false;
            }
            if (stream->serial.empty()) {
                log("device %d has no serial number; skipped", index);
                continue;
            }
            log("device %d: %s serial=%s depth-scale=%.9fm", index,
                stream->name.c_str(), stream->serial.c_str(),
                stream->depth_scale_m);
            devices.push_back(std::move(stream));
        }
        api.rs2_delete_device_list(list);

        if (static_cast<int>(devices.size()) < options.required_devices) {
            if (output) {
                *output = "expected " + std::to_string(options.required_devices) +
                    " D4xx devices, found " + std::to_string(devices.size());
            }
            return false;
        }
        if (!options.primary_serial.empty()) {
            const auto match = std::find_if(
                devices.begin(), devices.end(), [&](const auto& device) {
                    return device->serial == options.primary_serial;
                });
            if (match == devices.end()) {
                if (output) {
                    *output = "primary D4xx serial was not discovered: " +
                        options.primary_serial;
                }
                return false;
            }
            options.primary_device = static_cast<int>(
                std::distance(devices.begin(), match));
            log("selected primary device by serial: index=%d serial=%s",
                options.primary_device, options.primary_serial.c_str());
        }
        if (options.primary_device < 0 ||
            options.primary_device >= static_cast<int>(devices.size())) {
            if (output) *output = "primary D4xx device index is out of range";
            return false;
        }
        devices[static_cast<std::size_t>(options.primary_device)]->primary = true;
        log("transport recovery: stale after %d ms, reconnect every %d ms",
            options.frame_stall_timeout_ms, options.reconnect_delay_ms);

        int active = 0;
        for (auto& stream : devices) {
            std::string start_error;
            if (start_device(*stream, &start_error)) {
                ++active;
                log("streaming %s: %s, %dx%d@%d",
                    stream->serial.c_str(),
                    options.enable_infrared
                        ? "depth Z16 + infrared Y8"
                        : "depth Z16 only",
                    options.width, options.height, options.fps);
            } else {
                stream->reconnect_attempts = 1;
                stream->next_reconnect = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.reconnect_delay_ms);
                log("device %s failed: %s", stream->serial.c_str(),
                    start_error.c_str());
            }
        }
        if (active < options.required_devices) {
            if (output) {
                *output = "only " + std::to_string(active) + " of " +
                    std::to_string(options.required_devices) +
                    " required D4xx streams started";
            }
            return false;
        }

        worker = std::thread([this] { capture_loop(); });
        return true;
    }

    bool capture_frame(DeviceStream& stream, rs2_frame* composite) {
        rs2_error* error = nullptr;
        const int count = api.rs2_embedded_frames_count(composite, &error);
        if (!check(error, "rs2_embedded_frames_count")) return false;
        std::vector<std::uint8_t> infrared;
        std::vector<std::uint16_t> depth;
        rs2_intrinsics infrared_intrinsics{};
        rs2_intrinsics depth_intrinsics{};
        int infrared_width = 0;
        int infrared_height = 0;
        int depth_width = 0;
        int depth_height = 0;
        bool have_infrared = false;
        bool have_depth = false;
        bool have_intrinsics = false;
        bool have_depth_intrinsics = false;
        for (int index = 0; index < count; ++index) {
            rs2_frame* frame = api.rs2_extract_frame(composite, index, &error);
            if (!check(error, "rs2_extract_frame") || !frame) continue;
            const rs2_stream_profile* profile =
                api.rs2_get_frame_stream_profile(frame, &error);
            if (!check(error, "rs2_get_frame_stream_profile") || !profile) {
                api.rs2_release_frame(frame);
                continue;
            }
            rs2_stream stream_type = RS2_STREAM_ANY;
            rs2_format format = RS2_FORMAT_ANY;
            int stream_index = 0;
            int unique_id = 0;
            int fps = 0;
            api.rs2_get_stream_profile_data(
                profile, &stream_type, &format, &stream_index, &unique_id,
                &fps, &error);
            if (!check(error, "rs2_get_stream_profile_data")) {
                api.rs2_release_frame(frame);
                continue;
            }
            const int width = api.rs2_get_frame_width(frame, &error);
            if (!check(error, "rs2_get_frame_width")) {
                api.rs2_release_frame(frame);
                continue;
            }
            const int height = api.rs2_get_frame_height(frame, &error);
            if (!check(error, "rs2_get_frame_height")) {
                api.rs2_release_frame(frame);
                continue;
            }
            const int stride = api.rs2_get_frame_stride_in_bytes(frame, &error);
            if (!check(error, "rs2_get_frame_stride_in_bytes")) {
                api.rs2_release_frame(frame);
                continue;
            }
            const int bytes = api.rs2_get_frame_data_size(frame, &error);
            const void* data = api.rs2_get_frame_data(frame, &error);
            if (!check(error, "rs2_get_frame_data") || !data || width <= 0 ||
                height <= 0 || stride <= 0 || bytes < stride * height) {
                api.rs2_release_frame(frame);
                continue;
            }

            if (options.enable_infrared &&
                stream_type == RS2_STREAM_INFRARED &&
                stream_index == options.infrared_index &&
                format == RS2_FORMAT_Y8) {
                std::vector<std::uint8_t> copy(
                    static_cast<std::size_t>(width) * height);
                const auto* source = static_cast<const std::uint8_t*>(data);
                for (int row = 0; row < height; ++row) {
                    std::memcpy(copy.data() + static_cast<std::size_t>(row) * width,
                                source + static_cast<std::size_t>(row) * stride,
                                static_cast<std::size_t>(width));
                }
                rs2_intrinsics intrinsics{};
                api.rs2_get_video_stream_intrinsics(profile, &intrinsics, &error);
                const bool intrinsics_ok = check(
                    error, "rs2_get_video_stream_intrinsics");
                infrared.swap(copy);
                infrared_width = width;
                infrared_height = height;
                have_infrared = true;
                if (intrinsics_ok) {
                    infrared_intrinsics = intrinsics;
                    have_intrinsics = true;
                }
            } else if (stream_type == RS2_STREAM_DEPTH &&
                       format == RS2_FORMAT_Z16) {
                std::vector<std::uint16_t> copy(
                    static_cast<std::size_t>(width) * height);
                const auto* source = static_cast<const std::uint8_t*>(data);
                for (int row = 0; row < height; ++row) {
                    std::memcpy(
                        reinterpret_cast<std::uint8_t*>(copy.data()) +
                            static_cast<std::size_t>(row) * width * 2,
                        source + static_cast<std::size_t>(row) * stride,
                        static_cast<std::size_t>(width) * 2);
                }
                depth.swap(copy);
                depth_width = width;
                depth_height = height;
                have_depth = true;
                rs2_intrinsics intrinsics{};
                api.rs2_get_video_stream_intrinsics(profile, &intrinsics, &error);
                if (check(error, "rs2_get_video_stream_intrinsics(depth)")) {
                    depth_intrinsics = intrinsics;
                    have_depth_intrinsics = true;
                }
            }
            api.rs2_release_frame(frame);
        }
        if (!have_depth ||
            (options.enable_infrared &&
             (!have_infrared || infrared_width != depth_width ||
              infrared_height != depth_height)) ||
            (options.align_depth_to_infrared && !have_intrinsics) ||
            (!options.align_depth_to_infrared && !have_depth_intrinsics)) {
            return false;
        }
        // Publish one complete aligned frameset. Readers can no longer observe
        // a new IR frame paired with the preceding depth frame.
        std::lock_guard<std::mutex> lock(stream.frame_mutex);
        if (options.enable_infrared) stream.infrared.swap(infrared);
        else stream.infrared.clear();
        stream.depth.swap(depth);
        stream.intrinsics = options.align_depth_to_infrared
            ? infrared_intrinsics
            : depth_intrinsics;
        ++stream.sequence;
        stream.host_time_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        stream.width = options.enable_infrared
            ? infrared_width : depth_width;
        stream.height = options.enable_infrared
            ? infrared_height : depth_height;
        stream.have_intrinsics = true;
        stream.have_infrared = options.enable_infrared;
        stream.have_depth = true;
        return true;
    }

    bool align_frame(DeviceStream& stream, rs2_frame* input,
                     rs2_frame** output) {
        *output = nullptr;
        if (!stream.align || !stream.align_queue) return false;
        rs2_error* error = nullptr;
        // Processing blocks consume one reference. Preserve the pipeline's
        // reference because the caller still releases it after alignment.
        api.rs2_frame_add_ref(input, &error);
        if (!check(error, "rs2_frame_add_ref")) return false;
        api.rs2_process_frame(stream.align, input, &error);
        if (!check(error, "rs2_process_frame(align infrared)")) return false;
        const int available = api.rs2_poll_for_frame(
            stream.align_queue, output, &error);
        return check(error, "rs2_poll_for_frame(align infrared)") &&
            available != 0 && *output != nullptr;
    }

    void capture_loop() {
        log("capture worker started (RGB disabled)");
        while (!stop) {
            bool received = false;
            for (auto& device : devices) {
                const auto now = std::chrono::steady_clock::now();
                if (!device->active.load(std::memory_order_acquire)) {
                    if (now >= device->next_reconnect) {
                        reconnect_device(*device);
                    }
                    continue;
                }
                rs2_frame* frames = nullptr;
                rs2_error* error = nullptr;
                const int available = api.rs2_pipeline_poll_for_frames(
                    device->pipeline, &frames, &error);
                if (error) {
                    const std::string detail = api.take_error(error);
                    make_device_unavailable(
                        *device, "frame poll failed: " + detail);
                    continue;
                }
                if (available && frames) {
                    bool published = false;
                    if (options.align_depth_to_infrared) {
                        rs2_frame* aligned = nullptr;
                        if (align_frame(*device, frames, &aligned)) {
                            published = capture_frame(*device, aligned);
                            api.rs2_release_frame(aligned);
                        }
                    } else {
                        published = capture_frame(*device, frames);
                    }
                    api.rs2_release_frame(frames);
                    if (published) {
                        device->last_complete_frame = now;
                        if (device->reconnect_attempts != 0) {
                            log("device %s resumed complete frames",
                                device->serial.c_str());
                            device->reconnect_attempts = 0;
                        }
                        received = true;
                    }
                }
                if (device->active.load(std::memory_order_acquire) &&
                    now - device->last_complete_frame >=
                        std::chrono::milliseconds(
                            options.frame_stall_timeout_ms)) {
                    make_device_unavailable(
                        *device, "no complete frame for " +
                            std::to_string(options.frame_stall_timeout_ms) +
                            " ms");
                }
            }
            if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        log("capture worker stopped");
    }

    void close() {
        stop = true;
        if (worker.joinable()) worker.join();
        for (auto& device : devices) cleanup_device(*device);
        devices.clear();
        if (context) api.rs2_delete_context(context);
        context = nullptr;
        api.unload();
    }
};

std::shared_ptr<D4xxSource> D4xxSource::acquire(
    const D4xxOptions& options, std::string* error) {
    auto source = std::shared_ptr<D4xxSource>(new D4xxSource(options));
    if (!source->impl_->open(error)) return {};
    return source;
}

D4xxSource::D4xxSource(const D4xxOptions& options)
    : impl_(new Impl(options)) {}

D4xxSource::~D4xxSource() {
    if (impl_) impl_->close();
    delete impl_;
}

bool D4xxSource::wait_until_ready(int timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const int primary = impl_->options.primary_device;
        if (primary >= 0 && primary < static_cast<int>(impl_->devices.size()) &&
            impl_->devices[static_cast<std::size_t>(primary)]->active.load(
                std::memory_order_acquire) &&
            (!impl_->options.enable_infrared ||
             impl_->devices[static_cast<std::size_t>(primary)]->have_infrared) &&
            impl_->devices[static_cast<std::size_t>(primary)]->have_depth) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool D4xxSource::get_frame_bgr8(std::vector<std::uint8_t>& out,
                                int width, int height) {
    const int primary = impl_->options.primary_device;
    if (primary < 0 || primary >= static_cast<int>(impl_->devices.size())) {
        out.clear();
        return false;
    }
    const auto& stream = impl_->devices[static_cast<std::size_t>(primary)];
    if (!stream->active.load(std::memory_order_acquire)) {
        out.clear();
        return false;
    }
    std::vector<std::uint8_t> infrared;
    int source_width = 0;
    int source_height = 0;
    {
        std::lock_guard<std::mutex> lock(stream->frame_mutex);
        if (!stream->have_infrared || stream->infrared.empty()) {
            out.clear();
            return false;
        }
        infrared = stream->infrared;
        source_width = stream->width;
        source_height = stream->height;
    }
    std::vector<std::uint8_t> native_bgr;
    anygear::d4xx::ir_y8_to_bgr24(
        infrared.data(), infrared.size(), source_width, source_height,
        source_width, {}, &native_bgr);
    if (source_width == width && source_height == height) {
        out.swap(native_bgr);
    } else {
        out.resize(static_cast<std::size_t>(width) * height * 3);
        scale_bgr8_bilinear(native_bgr.data(), source_width, source_height,
                            out.data(), width, height);
    }
    return true;
}

bool D4xxSource::get_pose_frame_bgr8(std::vector<std::uint8_t>& out,
                                     int width, int height) {
    const int primary = impl_->options.primary_device;
    if (primary < 0 || primary >= static_cast<int>(impl_->devices.size())) {
        out.clear();
        return false;
    }
    const auto& stream = impl_->devices[static_cast<std::size_t>(primary)];
    if (!stream->active.load(std::memory_order_acquire)) {
        out.clear();
        return false;
    }
    std::vector<std::uint8_t> infrared;
    std::vector<std::uint16_t> depth;
    rs2_intrinsics intrinsics{};
    float depth_scale_m = impl_->options.depth_scale_m;
    int source_width = 0;
    int source_height = 0;
    {
        std::lock_guard<std::mutex> lock(stream->frame_mutex);
        if (!stream->have_infrared || !stream->have_depth ||
            !stream->have_intrinsics || stream->infrared.empty() ||
            stream->depth.empty()) {
            out.clear();
            return false;
        }
        infrared = stream->infrared;
        depth = stream->depth;
        intrinsics = stream->intrinsics;
        depth_scale_m = stream->depth_scale_m;
        source_width = stream->width;
        source_height = stream->height;
    }
    if (source_width <= 0 || source_height <= 0 ||
        depth.size() != static_cast<std::size_t>(source_width) *
                            source_height) {
        out.clear();
        return false;
    }

    std::vector<std::uint8_t> native_bgr;
    anygear::d4xx::ir_y8_to_bgr24(
        infrared.data(), infrared.size(), source_width, source_height,
        source_width, {}, &native_bgr);
    if (source_width == width && source_height == height) {
        out.swap(native_bgr);
    } else {
        out.resize(static_cast<std::size_t>(width) * height * 3);
        scale_bgr8_bilinear(native_bgr.data(), source_width, source_height,
                            out.data(), width, height);
    }

    // Only the analysis thread calls this method. Keep the exact aligned depth
    // packet alive while MediaPipe works; preview reads cannot replace it.
    std::lock_guard<std::mutex> pose_lock(impl_->pose_mutex);
    impl_->pending_pose_depth.swap(depth);
    impl_->pending_pose_depth_scale_m = depth_scale_m;
    impl_->pending_pose_intrinsics = intrinsics;
    impl_->pending_pose_width = source_width;
    impl_->pending_pose_height = source_height;
    impl_->pending_pose_pixel_width = width;
    impl_->pending_pose_pixel_height = height;
    impl_->have_pending_pose_depth = true;
    return true;
}

bool D4xxSource::apply_depth_to_pose(PoseResult* pose) {
    if (!pose || !pose->valid) return false;
    const int primary = impl_->options.primary_device;
    if (primary < 0 || primary >= static_cast<int>(impl_->devices.size())) {
        return false;
    }
    const auto& stream = impl_->devices[static_cast<std::size_t>(primary)];
    std::vector<std::uint16_t> depth;
    rs2_intrinsics intrinsics{};
    float depth_scale_m = impl_->options.depth_scale_m;
    int width = 0;
    int height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
    {
        std::lock_guard<std::mutex> pose_lock(impl_->pose_mutex);
        if (impl_->have_pending_pose_depth) {
            depth.swap(impl_->pending_pose_depth);
            depth_scale_m = impl_->pending_pose_depth_scale_m;
            intrinsics = impl_->pending_pose_intrinsics;
            width = impl_->pending_pose_width;
            height = impl_->pending_pose_height;
            pixel_width = impl_->pending_pose_pixel_width;
            pixel_height = impl_->pending_pose_pixel_height;
            impl_->have_pending_pose_depth = false;
        }
    }
    if (depth.empty()) {
        // Retain compatibility for callers that only request preview frames.
        std::lock_guard<std::mutex> lock(stream->frame_mutex);
        if (!stream->active.load(std::memory_order_acquire) ||
            !stream->have_depth || !stream->have_intrinsics ||
            stream->depth.empty()) {
            return false;
        }
        depth = stream->depth;
        depth_scale_m = stream->depth_scale_m;
        intrinsics = stream->intrinsics;
        width = stream->width;
        height = stream->height;
        pixel_width = width;
        pixel_height = height;
    }
    if (width <= 0 || height <= 0 || intrinsics.fx <= 0.0f ||
        intrinsics.fy <= 0.0f ||
        depth.size() != static_cast<std::size_t>(width) * height) {
        return false;
    }

    std::array<std::array<float, 3>, kMpLandmarkCount> camera{};
    std::array<bool, kMpLandmarkCount> measured{};
    for (int index = 0; index < kMpLandmarkCount; ++index) {
        const float depth_x = pose->pixel[index][0] * width /
            static_cast<float>(std::max(1, pixel_width));
        const float depth_y = pose->pixel[index][1] * height /
            static_cast<float>(std::max(1, pixel_height));
        const float z = median_depth(
            depth, width, height, depth_x, depth_y,
            depth_scale_m);
        if (z <= 0.0f) continue;
        const float x =
            (depth_x - intrinsics.ppx) / intrinsics.fx * z;
        const float y_down =
            (depth_y - intrinsics.ppy) / intrinsics.fy * z;
        camera[static_cast<std::size_t>(index)] = {x, -y_down, z};
        measured[static_cast<std::size_t>(index)] = true;
    }

    const auto average_pair = [&](int left, int right, int component,
                                  float* value) {
        float sum = 0.0f;
        int count = 0;
        if (measured[static_cast<std::size_t>(left)]) {
            sum += camera[static_cast<std::size_t>(left)]
                         [static_cast<std::size_t>(component)];
            ++count;
        }
        if (measured[static_cast<std::size_t>(right)]) {
            sum += camera[static_cast<std::size_t>(right)]
                         [static_cast<std::size_t>(component)];
            ++count;
        }
        if (count == 0) return false;
        *value = sum / static_cast<float>(count);
        return true;
    };

    float hip_camera[3]{};
    if (!average_pair(MP_LeftHip, MP_RightHip, 0, &hip_camera[0]) ||
        !average_pair(MP_LeftHip, MP_RightHip, 1, &hip_camera[1]) ||
        !average_pair(MP_LeftHip, MP_RightHip, 2, &hip_camera[2])) {
        return false;
    }
    const float media_hip[3] = {
        (pose->world[MP_LeftHip][0] + pose->world[MP_RightHip][0]) * 0.5f,
        (pose->world[MP_LeftHip][1] + pose->world[MP_RightHip][1]) * 0.5f,
        (pose->world[MP_LeftHip][2] + pose->world[MP_RightHip][2]) * 0.5f,
    };

    std::lock_guard<std::mutex> pose_lock(impl_->pose_mutex);
    float automatic_offset_z = 0.0f;
    if (impl_->options.auto_center_z) {
        if (!impl_->have_depth_anchor) {
            impl_->depth_anchor_samples.push_back(hip_camera[2]);
            constexpr std::size_t kAnchorSampleCount = 15;
            if (impl_->depth_anchor_samples.size() >= kAnchorSampleCount) {
                std::vector<float> sorted = impl_->depth_anchor_samples;
                std::sort(sorted.begin(), sorted.end());
                const float spread = sorted.back() - sorted.front();
                if (spread <= 0.25f) {
                    const float median = sorted[sorted.size() / 2];
                    impl_->depth_anchor_offset_z =
                        impl_->options.stage_hip_z - median;
                    impl_->have_depth_anchor = true;
                    impl_->log(
                        "placement: depth auto-center locked, measured=%.3fm "
                        "target=%.3fm correction=%+.3fm",
                        median, impl_->options.stage_hip_z,
                        impl_->depth_anchor_offset_z);
                } else {
                    impl_->depth_anchor_samples.erase(
                        impl_->depth_anchor_samples.begin());
                }
            }
        }
        automatic_offset_z = impl_->have_depth_anchor
            ? impl_->depth_anchor_offset_z
            : impl_->options.stage_hip_z - hip_camera[2];
    }

    const float degrees_to_radians =
        3.14159265358979323846f / 180.0f;
    const float pitch = impl_->options.pitch_degrees * degrees_to_radians;
    const float yaw = impl_->options.yaw_degrees * degrees_to_radians;
    const float roll = impl_->options.roll_degrees * degrees_to_radians;
    const float sin_pitch = std::sin(pitch);
    const float cos_pitch = std::cos(pitch);
    const float sin_yaw = std::sin(yaw);
    const float cos_yaw = std::cos(yaw);
    const float sin_roll = std::sin(roll);
    const float cos_roll = std::cos(roll);

    for (int index = 0; index < kMpLandmarkCount; ++index) {
        float raw[3]{};
        if (measured[static_cast<std::size_t>(index)]) {
            for (int component = 0; component < 3; ++component) {
                raw[component] = camera[static_cast<std::size_t>(index)]
                                       [static_cast<std::size_t>(component)];
            }
        } else {
            for (int component = 0; component < 3; ++component) {
                raw[component] = hip_camera[component] +
                    pose->world[index][component] - media_hip[component];
            }
            pose->vis[index] *= 0.75f;
        }

        // Correct camera mounting angles around the live hip. X is pitch,
        // Y is yaw, and Z is roll in the right-handed camera coordinate
        // system (+Y up, +Z away from the camera).
        float relative[3] = {
            raw[0] - hip_camera[0],
            raw[1] - hip_camera[1],
            raw[2] - hip_camera[2],
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
        raw[0] = hip_camera[0] + roll_x;
        raw[1] = hip_camera[1] + roll_y;
        raw[2] = hip_camera[2] + relative[2];

        float stage[3] = {raw[0], raw[1], raw[2]};
        if (impl_->options.face_to_face) {
            stage[0] = 2.0f * impl_->options.mirror_center_x - raw[0];
            stage[2] = 2.0f * hip_camera[2] - raw[2];
        }
        // Offsets are expressed in final stage space. Keeping them after the
        // face-to-face transform makes their direction independent of the
        // selected camera-facing convention.
        stage[0] += impl_->options.offset_x;
        stage[1] += impl_->options.offset_y;
        stage[2] += impl_->options.offset_z + automatic_offset_z;
        if (impl_->have_previous_pose) {
            const float alpha = std::clamp(impl_->options.smoothing, 0.0f, 1.0f);
            for (int component = 0; component < 3; ++component) {
                stage[component] = alpha * stage[component] +
                    (1.0f - alpha) * impl_->previous_pose[index][component];
            }
        }
        for (int component = 0; component < 3; ++component) {
            pose->world[index][component] = stage[component];
            impl_->previous_pose[index][component] = stage[component];
        }
    }
    impl_->have_previous_pose = true;
    return true;
}

bool D4xxSource::get_depth_frame(int device_index,
                                 std::uint64_t after_sequence,
                                 D4xxDepthFrame* out) const {
    if (!out || device_index < 0 ||
        device_index >= static_cast<int>(impl_->devices.size())) {
        return false;
    }
    const auto& stream = impl_->devices[static_cast<std::size_t>(device_index)];
    std::lock_guard<std::mutex> lock(stream->frame_mutex);
    if (!stream->active.load(std::memory_order_acquire) ||
        !stream->have_depth || !stream->have_intrinsics ||
        stream->depth.empty() || stream->sequence <= after_sequence) {
        return false;
    }
    out->device_index = device_index;
    out->serial = stream->serial;
    out->sequence = stream->sequence;
    out->host_time_ns = stream->host_time_ns;
    out->depth_scale_m = stream->depth_scale_m;
    out->intrinsics.width = stream->width;
    out->intrinsics.height = stream->height;
    out->intrinsics.principal_x = stream->intrinsics.ppx;
    out->intrinsics.principal_y = stream->intrinsics.ppy;
    out->intrinsics.focal_x = stream->intrinsics.fx;
    out->intrinsics.focal_y = stream->intrinsics.fy;
    out->intrinsics.distortion_model =
        static_cast<int>(stream->intrinsics.model);
    std::copy(std::begin(stream->intrinsics.coeffs),
              std::end(stream->intrinsics.coeffs),
              std::begin(out->intrinsics.distortion));
    out->depth = stream->depth;
    return true;
}

int D4xxSource::device_count() const {
    return static_cast<int>(impl_->devices.size());
}

int D4xxSource::active_device_count() const {
    return static_cast<int>(std::count_if(
        impl_->devices.begin(), impl_->devices.end(),
        [](const std::unique_ptr<Impl::DeviceStream>& value) {
            return value->active.load(std::memory_order_acquire);
        }));
}

bool D4xxSource::left_active() const {
    return active_device_count() >= 1;
}

bool D4xxSource::right_active() const {
    return active_device_count() >= 2;
}

int D4xxSource::native_width() const {
    const int primary = impl_->options.primary_device;
    return primary >= 0 && primary < static_cast<int>(impl_->devices.size())
        ? impl_->devices[static_cast<std::size_t>(primary)]->width.load() : 0;
}

int D4xxSource::native_height() const {
    const int primary = impl_->options.primary_device;
    return primary >= 0 && primary < static_cast<int>(impl_->devices.size())
        ? impl_->devices[static_cast<std::size_t>(primary)]->height.load() : 0;
}

std::vector<std::string> D4xxSource::serial_numbers() const {
    std::vector<std::string> result;
    result.reserve(impl_->devices.size());
    for (const auto& device : impl_->devices) result.push_back(device->serial);
    return result;
}

} // namespace vp4u
