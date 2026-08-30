// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal game-owned VP4U shape for the native bridge integration test.

#include <algorithm>
#include <cstdint>

#include "vp4u/vp4u_abi.h"

#include <librealsense2/rs.h>
#include <librealsense2/h/rs_config.h>
#include <librealsense2/h/rs_frame.h>
#include <librealsense2/h/rs_processing.h>
#include <librealsense2/h/rs_sensor.h>

namespace {

int fake_init_realtime(const char*) {
    return 7;
}

int fake_init_offline(const char*) {
    return -12;
}

bool fake_start_offline(
    const char*, vp4u::OnPoseFrameCallback, void*) {
    return false;
}

void fake_stop_offline() {}

} // namespace

namespace vp4u {

extern "C" __declspec(dllexport) int vp4uGetVersion() {
    return kApiVersion;
}

extern "C" __declspec(dllexport) bool vp4uPreboot(
    const char*, OnLogCallback, void*) {
    return true;
}

extern "C" __declspec(dllexport) void vp4uShutdown() {}

extern "C" __declspec(dllexport) bool vp4uGetApiTable(
    int api_version, FunctionPointerTable* table,
    OnLogCallback, void*) {
    if (api_version != kApiVersion || !table) return false;
    *table = {};
    table->InitVisionPoseRealtime = fake_init_realtime;
    table->InitVisionPoseOffline = fake_init_offline;
    table->StartOfflineAnalysis = fake_start_offline;
    table->StopOfflineAnalysis = fake_stop_offline;
    return true;
}

} // namespace vp4u

extern "C" __declspec(dllexport) int anygearFakeExerciseBridge(
    std::uint8_t* output, int capacity, int* metadata, int metadata_count) {
    if (!output || capacity < 12 || !metadata || metadata_count < 8) return 0;
    rs2_error* error = nullptr;
    static constexpr char advanced_camera_json[] =
        "{\"controls-color-autoexposure-auto\":\"False\","
        "\"controls-laserpower\":\"240\"}";
    rs2_load_json(
        reinterpret_cast<rs2_device*>(3), advanced_camera_json,
        static_cast<unsigned int>(sizeof(advanced_camera_json) - 1), &error);
    rs2_config_enable_stream(
        reinterpret_cast<rs2_config*>(1), RS2_STREAM_COLOR, -1,
        2, 2, RS2_FORMAT_BGR8, 60, &error);
    rs2_processing_block* align = rs2_create_align(RS2_STREAM_COLOR, &error);
    const auto* frame = reinterpret_cast<const rs2_frame*>(2);
    const rs2_stream_profile* profile =
        rs2_get_frame_stream_profile(frame, &error);
    rs2_stream stream = RS2_STREAM_ANY;
    rs2_format format = RS2_FORMAT_ANY;
    int index = -1;
    int unique_id = 0;
    int framerate = 0;
    rs2_get_stream_profile_data(
        profile, &stream, &format, &index, &unique_id, &framerate, &error);
    const int width = rs2_get_frame_width(frame, &error);
    const int height = rs2_get_frame_height(frame, &error);
    const int size = rs2_get_frame_data_size(frame, &error);
    const int stride = rs2_get_frame_stride_in_bytes(frame, &error);
    const int bits = rs2_get_frame_bits_per_pixel(frame, &error);
    const auto* data = static_cast<const std::uint8_t*>(
        rs2_get_frame_data(frame, &error));
    if (error) {
        rs2_free_error(error);
        return 0;
    }
    if (!align || !data || size > capacity) return 0;
    std::copy(data, data + size, output);
    metadata[0] = static_cast<int>(stream);
    metadata[1] = static_cast<int>(format);
    metadata[2] = index;
    metadata[3] = width;
    metadata[4] = height;
    metadata[5] = size;
    metadata[6] = stride;
    metadata[7] = bits;
    rs2_release_frame(const_cast<rs2_frame*>(frame));
    return size;
}

extern "C" __declspec(dllexport) int anygearFakeColorSensorCompatible() {
    rs2_error* error = nullptr;
    return rs2_is_sensor_extendable_to(
        reinterpret_cast<const rs2_sensor*>(4),
        RS2_EXTENSION_COLOR_SENSOR, &error);
}
