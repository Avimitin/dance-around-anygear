// SPDX-License-Identifier: GPL-3.0-or-later
// Verifies D4xx stale-frame invalidation and serial-bound pipeline recovery.

#include "vp4u/d4xx.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>

namespace vp4u {

// d4xx.cpp also contains the legacy infrared preview path. The recovery test
// exercises depth-only capture, so a scaler implementation is not required.
void scale_bgr8_bilinear(
    const std::uint8_t*, int, int, std::uint8_t*, int, int) {}

} // namespace vp4u

namespace {

template <typename T>
T load_proc(HMODULE module, const char* name) {
    const FARPROC raw = GetProcAddress(module, name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

bool expect(bool value, const char* message) {
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool wait_for(const std::function<bool()>& condition, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr,
            "usage: d4xx_reconnect_test <synthetic-realsense2.dll>\n");
        return 64;
    }
    std::filesystem::path runtime =
        std::filesystem::absolute(std::filesystem::path(argv[1]));
    runtime.make_preferred();
    HMODULE fake = LoadLibraryExW(
        runtime.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!expect(fake != nullptr, "synthetic librealsense did not load")) {
        return 1;
    }

    using SetFlag = void (__cdecl*)(int);
    using Action = void (__cdecl*)();
    using GetInt = int (__cdecl*)();
    const auto reset = load_proc<Action>(fake, "fakeRsResetCaptureState");
    const auto capture_mode =
        load_proc<SetFlag>(fake, "fakeRsEnableCaptureMode");
    const auto enable_frames =
        load_proc<SetFlag>(fake, "fakeRsEnableFrames");
    const auto fail_next_poll =
        load_proc<Action>(fake, "fakeRsFailNextPoll");
    const auto pipeline_starts =
        load_proc<GetInt>(fake, "fakeRsPipelineStartCount");
    const auto option_sets =
        load_proc<GetInt>(fake, "fakeRsOptionSetCount");
    if (!expect(reset && capture_mode && enable_frames && fail_next_poll &&
                    pipeline_starts && option_sets,
                "synthetic librealsense controls are incomplete")) {
        FreeLibrary(fake);
        return 1;
    }
    reset();
    capture_mode(1);

    vp4u::D4xxOptions options;
    options.runtime_path = runtime.wstring();
    options.width = 2;
    options.height = 2;
    options.fps = 30;
    options.required_devices = 1;
    options.enable_infrared = false;
    options.align_depth_to_infrared = false;
    options.frame_stall_timeout_ms = 150;
    options.reconnect_delay_ms = 100;
    options.visual_preset_by_device = {4};
    options.emitter_enabled_by_device = {1};
    options.emitter_on_off_by_device = {0};

    std::string error;
    auto source = vp4u::D4xxSource::acquire(options, &error);
    if (!expect(source != nullptr, error.c_str()) ||
        !expect(source->wait_until_ready(1000),
                "initial depth frame did not arrive")) {
        source.reset();
        FreeLibrary(fake);
        return 2;
    }
    vp4u::D4xxDepthFrame initial;
    if (!expect(source->get_depth_frame(0, 0, &initial),
                "initial depth packet was not published") ||
        !expect(initial.depth.size() == 4 && initial.depth[0] == 1000,
                "initial depth packet content is wrong") ||
        !expect(pipeline_starts() == 1 && option_sets() >= 4,
                "initial pipeline/options were not configured")) {
        source.reset();
        FreeLibrary(fake);
        return 2;
    }

    const int initial_option_sets = option_sets();
    fail_next_poll();
    if (!expect(wait_for([&] { return source->active_device_count() == 0; },
                         500),
                "poll failure did not invalidate the stream")) {
        source.reset();
        FreeLibrary(fake);
        return 3;
    }
    vp4u::D4xxDepthFrame stale;
    if (!expect(!source->get_depth_frame(0, 0, &stale),
                "last depth packet remained readable while disconnected") ||
        !expect(wait_for([&] {
                    return pipeline_starts() >= 2 &&
                        source->active_device_count() == 1;
                }, 1000),
                "pipeline did not recover after a poll failure") ||
        !expect(option_sets() >= initial_option_sets + 4,
                "camera options were not restored during reconnect")) {
        source.reset();
        FreeLibrary(fake);
        return 3;
    }
    vp4u::D4xxDepthFrame after_error;
    if (!expect(wait_for([&] {
                    return source->get_depth_frame(
                        0, initial.sequence, &after_error);
                }, 500),
                "no fresh packet arrived after poll recovery")) {
        source.reset();
        FreeLibrary(fake);
        return 3;
    }

    enable_frames(0);
    if (!expect(wait_for([&] { return source->active_device_count() == 0; },
                         750),
                "silent frame stall did not invalidate the stream") ||
        !expect(!source->get_depth_frame(0, 0, &stale),
                "stalled stream exposed its last depth packet")) {
        source.reset();
        FreeLibrary(fake);
        return 4;
    }
    enable_frames(1);
    vp4u::D4xxDepthFrame after_stall;
    if (!expect(wait_for([&] {
                    return pipeline_starts() >= 3 &&
                        source->get_depth_frame(
                            0, after_error.sequence, &after_stall);
                }, 1000),
                "pipeline did not recover after a silent frame stall")) {
        source.reset();
        FreeLibrary(fake);
        return 4;
    }

    source.reset();
    capture_mode(0);
    FreeLibrary(fake);
    std::fprintf(stderr,
        "PASS: D4xx poll errors and frame stalls invalidate and recover\n");
    return 0;
}
