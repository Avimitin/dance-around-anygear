// SPDX-License-Identifier: GPL-3.0-or-later
// Source-only librealsense stand-in used to verify the native bridge offline.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <windows.h>

#include <librealsense2/rs.h>
#include <librealsense2/h/rs_config.h>
#include <librealsense2/h/rs_frame.h>
#include <librealsense2/h/rs_processing.h>
#include <librealsense2/h/rs_sensor.h>

namespace {

rs2_stream g_last_config_stream = RS2_STREAM_ANY;
rs2_format g_last_config_format = RS2_FORMAT_ANY;
int g_last_config_index = -1;
rs2_stream g_last_align_stream = RS2_STREAM_ANY;
std::atomic<int> g_release_count{0};
std::string g_last_json;
std::array<std::uint8_t, 4> g_ir{{0, 64, 128, 255}};
std::array<std::uint16_t, 4> g_depth{{1000, 1100, 1200, 1300}};
std::uintptr_t g_profile_storage = 0;
std::uintptr_t g_processing_storage = 0;
std::uintptr_t g_context_storage = 0;
std::uintptr_t g_device_list_storage = 0;
std::uintptr_t g_device_storage = 0;
std::uintptr_t g_sensor_list_storage = 0;
std::uintptr_t g_sensor_storage = 0;
std::uintptr_t g_pipeline_storage = 0;
std::uintptr_t g_config_storage = 0;
std::uintptr_t g_pipeline_profile_storage = 0;
std::uintptr_t g_queue_storage = 0;
std::uintptr_t g_frame_storage = 0;
std::uintptr_t g_error_storage = 0;
std::atomic<bool> g_capture_mode{false};
std::atomic<bool> g_pipeline_running{false};
std::atomic<bool> g_frames_enabled{true};
std::atomic<int> g_fail_next_poll{0};
std::atomic<int> g_pipeline_start_count{0};
std::atomic<int> g_option_set_count{0};

template <typename T>
T* token(std::uintptr_t* storage) {
    return reinterpret_cast<T*>(storage);
}

void set_error(rs2_error** error) {
    if (error) *error = token<rs2_error>(&g_error_storage);
}

} // namespace

extern "C" __declspec(dllexport) int rs2_get_api_version(rs2_error**) {
    return RS2_API_VERSION;
}

extern "C" __declspec(dllexport) const char* rs2_get_error_message(
    const rs2_error*) {
    return "synthetic USB transport error";
}

extern "C" __declspec(dllexport) rs2_context* rs2_create_context(
    int, rs2_error**) {
    return token<rs2_context>(&g_context_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_context(rs2_context*) {}

extern "C" __declspec(dllexport) rs2_device_list* rs2_query_devices(
    const rs2_context*, rs2_error**) {
    return token<rs2_device_list>(&g_device_list_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_device_list(
    rs2_device_list*) {}

extern "C" __declspec(dllexport) int rs2_get_device_count(
    const rs2_device_list*, rs2_error**) {
    return 1;
}

extern "C" __declspec(dllexport) rs2_device* rs2_create_device(
    const rs2_device_list*, int index, rs2_error** error) {
    if (index != 0) {
        set_error(error);
        return nullptr;
    }
    return token<rs2_device>(&g_device_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_device(rs2_device*) {}

extern "C" __declspec(dllexport) int rs2_supports_device_info(
    const rs2_device*, rs2_camera_info info, rs2_error**) {
    return info == RS2_CAMERA_INFO_NAME ||
           info == RS2_CAMERA_INFO_SERIAL_NUMBER;
}

extern "C" __declspec(dllexport) const char* rs2_get_device_info(
    const rs2_device*, rs2_camera_info info, rs2_error**) {
    if (info == RS2_CAMERA_INFO_NAME) return "Synthetic RealSense D430";
    if (info == RS2_CAMERA_INFO_SERIAL_NUMBER) return "FAKE-D430-001";
    return "";
}

extern "C" __declspec(dllexport) rs2_sensor_list* rs2_query_sensors(
    const rs2_device*, rs2_error**) {
    return token<rs2_sensor_list>(&g_sensor_list_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_sensor_list(
    rs2_sensor_list*) {}

extern "C" __declspec(dllexport) int rs2_get_sensors_count(
    const rs2_sensor_list*, rs2_error**) {
    return 1;
}

extern "C" __declspec(dllexport) rs2_sensor* rs2_create_sensor(
    const rs2_sensor_list*, int index, rs2_error** error) {
    if (index != 0) {
        set_error(error);
        return nullptr;
    }
    return token<rs2_sensor>(&g_sensor_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_sensor(rs2_sensor*) {}

extern "C" __declspec(dllexport) float rs2_get_depth_scale(
    rs2_sensor*, rs2_error**) {
    return 0.001f;
}

extern "C" __declspec(dllexport) int rs2_supports_option(
    const rs2_options*, rs2_option, rs2_error**) {
    return 1;
}

extern "C" __declspec(dllexport) float rs2_get_option(
    const rs2_options*, rs2_option, rs2_error**) {
    return 1.0f;
}

extern "C" __declspec(dllexport) void rs2_set_option(
    const rs2_options*, rs2_option, float, rs2_error**) {
    ++g_option_set_count;
}

extern "C" __declspec(dllexport) rs2_pipeline* rs2_create_pipeline(
    rs2_context*, rs2_error**) {
    return token<rs2_pipeline>(&g_pipeline_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_pipeline(rs2_pipeline*) {}

extern "C" __declspec(dllexport) void rs2_pipeline_stop(
    rs2_pipeline*, rs2_error**) {
    g_pipeline_running = false;
}

extern "C" __declspec(dllexport) int rs2_pipeline_poll_for_frames(
    rs2_pipeline*, rs2_frame** output, rs2_error** error) {
    if (!g_pipeline_running.load()) {
        set_error(error);
        if (output) *output = nullptr;
        return 0;
    }
    if (g_fail_next_poll.exchange(0) != 0) {
        g_pipeline_running = false;
        set_error(error);
        if (output) *output = nullptr;
        return 0;
    }
    if (!g_frames_enabled.load()) {
        if (output) *output = nullptr;
        return 0;
    }
    Sleep(2);
    if (output) *output = token<rs2_frame>(&g_frame_storage);
    return 1;
}

extern "C" __declspec(dllexport) rs2_pipeline_profile*
rs2_pipeline_start_with_config(rs2_pipeline*, rs2_config*, rs2_error**) {
    g_pipeline_running = true;
    ++g_pipeline_start_count;
    return token<rs2_pipeline_profile>(&g_pipeline_profile_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_pipeline_profile(
    rs2_pipeline_profile*) {}

extern "C" __declspec(dllexport) rs2_frame_queue* rs2_create_frame_queue(
    int, rs2_error**) {
    return token<rs2_frame_queue>(&g_queue_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_frame_queue(
    rs2_frame_queue*) {}

extern "C" __declspec(dllexport) int rs2_poll_for_frame(
    rs2_frame_queue*, rs2_frame** output, rs2_error**) {
    if (output) *output = token<rs2_frame>(&g_frame_storage);
    return 1;
}

extern "C" __declspec(dllexport) void rs2_frame_add_ref(
    rs2_frame*, rs2_error**) {}

extern "C" __declspec(dllexport) void rs2_start_processing_queue(
    rs2_processing_block*, rs2_frame_queue*, rs2_error**) {}

extern "C" __declspec(dllexport) void rs2_process_frame(
    rs2_processing_block*, rs2_frame*, rs2_error**) {}

extern "C" __declspec(dllexport) void rs2_delete_processing_block(
    rs2_processing_block*) {}

extern "C" __declspec(dllexport) rs2_config* rs2_create_config(rs2_error**) {
    return token<rs2_config>(&g_config_storage);
}

extern "C" __declspec(dllexport) void rs2_delete_config(rs2_config*) {}

extern "C" __declspec(dllexport) void rs2_config_enable_device(
    rs2_config*, const char*, rs2_error**) {}

extern "C" __declspec(dllexport) void rs2_config_enable_stream(
    rs2_config*, rs2_stream stream, int index, int, int,
    rs2_format format, int, rs2_error**) {
    g_last_config_stream = stream;
    g_last_config_format = format;
    g_last_config_index = index;
}

extern "C" __declspec(dllexport) int rs2_embedded_frames_count(
    rs2_frame*, rs2_error**) {
    return 1;
}

extern "C" __declspec(dllexport) rs2_frame* rs2_extract_frame(
    rs2_frame*, int index, rs2_error** error) {
    if (index != 0) {
        set_error(error);
        return nullptr;
    }
    return token<rs2_frame>(&g_frame_storage);
}

extern "C" __declspec(dllexport) rs2_processing_block* rs2_create_align(
    rs2_stream align_to, rs2_error**) {
    g_last_align_stream = align_to;
    return reinterpret_cast<rs2_processing_block*>(&g_processing_storage);
}

extern "C" __declspec(dllexport) void rs2_load_json(
    rs2_device*, const void* json_content, unsigned int content_size,
    rs2_error**) {
    g_last_json.assign(
        static_cast<const char*>(json_content),
        static_cast<std::size_t>(content_size));
}

extern "C" __declspec(dllexport) int rs2_is_sensor_extendable_to(
    const rs2_sensor*, rs2_extension extension, rs2_error**) {
    return extension == RS2_EXTENSION_DEPTH_SENSOR ? 1 : 0;
}

extern "C" __declspec(dllexport) void rs2_get_stream_profile_data(
    const rs2_stream_profile*, rs2_stream* stream, rs2_format* format,
    int* index, int* unique_id, int* framerate, rs2_error**) {
    if (stream) {
        *stream = g_capture_mode.load()
            ? RS2_STREAM_DEPTH : RS2_STREAM_INFRARED;
    }
    if (format) {
        *format = g_capture_mode.load() ? RS2_FORMAT_Z16 : RS2_FORMAT_Y8;
    }
    if (index) *index = g_capture_mode.load() ? 0 : 1;
    if (unique_id) *unique_id = 42;
    if (framerate) *framerate = 60;
}

extern "C" __declspec(dllexport) const rs2_stream_profile*
rs2_get_frame_stream_profile(const rs2_frame*, rs2_error**) {
    return reinterpret_cast<const rs2_stream_profile*>(&g_profile_storage);
}

extern "C" __declspec(dllexport) const void* rs2_get_frame_data(
    const rs2_frame*, rs2_error**) {
    return g_capture_mode.load()
        ? static_cast<const void*>(g_depth.data())
        : static_cast<const void*>(g_ir.data());
}

extern "C" __declspec(dllexport) int rs2_get_frame_data_size(
    const rs2_frame*, rs2_error**) {
    return g_capture_mode.load()
        ? static_cast<int>(g_depth.size() * sizeof(g_depth[0]))
        : static_cast<int>(g_ir.size());
}

extern "C" __declspec(dllexport) int rs2_get_frame_width(
    const rs2_frame*, rs2_error**) {
    return 2;
}

extern "C" __declspec(dllexport) int rs2_get_frame_height(
    const rs2_frame*, rs2_error**) {
    return 2;
}

extern "C" __declspec(dllexport) int rs2_get_frame_stride_in_bytes(
    const rs2_frame*, rs2_error**) {
    return g_capture_mode.load() ? 4 : 2;
}

extern "C" __declspec(dllexport) int rs2_get_frame_bits_per_pixel(
    const rs2_frame*, rs2_error**) {
    return g_capture_mode.load() ? 16 : 8;
}

extern "C" __declspec(dllexport) void rs2_get_video_stream_intrinsics(
    const rs2_stream_profile*, rs2_intrinsics* intrinsics, rs2_error**) {
    if (!intrinsics) return;
    std::memset(intrinsics, 0, sizeof(*intrinsics));
    intrinsics->width = 2;
    intrinsics->height = 2;
    intrinsics->ppx = 0.5f;
    intrinsics->ppy = 0.5f;
    intrinsics->fx = 1.0f;
    intrinsics->fy = 1.0f;
    intrinsics->model = RS2_DISTORTION_NONE;
}

extern "C" __declspec(dllexport) void rs2_release_frame(rs2_frame*) {
    ++g_release_count;
}

extern "C" __declspec(dllexport) void rs2_free_error(rs2_error*) {}

extern "C" __declspec(dllexport) int fakeRsLastConfigStream() {
    return static_cast<int>(g_last_config_stream);
}

extern "C" __declspec(dllexport) int fakeRsLastConfigFormat() {
    return static_cast<int>(g_last_config_format);
}

extern "C" __declspec(dllexport) int fakeRsLastConfigIndex() {
    return g_last_config_index;
}

extern "C" __declspec(dllexport) int fakeRsLastAlignStream() {
    return static_cast<int>(g_last_align_stream);
}

extern "C" __declspec(dllexport) int fakeRsReleaseCount() {
    return g_release_count.load();
}

extern "C" __declspec(dllexport) void fakeRsEnableCaptureMode(int enabled) {
    g_capture_mode = enabled != 0;
}

extern "C" __declspec(dllexport) void fakeRsFailNextPoll() {
    g_fail_next_poll = 1;
}

extern "C" __declspec(dllexport) void fakeRsEnableFrames(int enabled) {
    g_frames_enabled = enabled != 0;
}

extern "C" __declspec(dllexport) int fakeRsPipelineStartCount() {
    return g_pipeline_start_count.load();
}

extern "C" __declspec(dllexport) int fakeRsOptionSetCount() {
    return g_option_set_count.load();
}

extern "C" __declspec(dllexport) void fakeRsResetCaptureState() {
    g_capture_mode = false;
    g_pipeline_running = false;
    g_frames_enabled = true;
    g_fail_next_poll = 0;
    g_pipeline_start_count = 0;
    g_option_set_count = 0;
}

extern "C" __declspec(dllexport) int fakeRsJsonContainsColorControls() {
    return g_last_json.find("controls-color-") != std::string::npos;
}

extern "C" __declspec(dllexport) int fakeRsJsonContainsDepthControls() {
    return g_last_json.find("controls-laserpower") != std::string::npos;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
