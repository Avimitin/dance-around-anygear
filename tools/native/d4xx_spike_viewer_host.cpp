// Standalone D4xx capture host for the browser-based SPiKE diagnostics viewer.
#include "d4xx/spike_ipc.h"
#include "vp4u/d4xx.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <shellapi.h>
#include <windows.h>

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI console_control_handler(DWORD control) {
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
        control == CTRL_CLOSE_EVENT || control == CTRL_SHUTDOWN_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

struct Options {
    std::filesystem::path realsense_runtime;
    std::filesystem::path worker_executable;
    std::filesystem::path model;
    std::filesystem::path config;
    std::filesystem::path log;
    int port = 8765;
    bool open_browser = true;
};

void print_usage() {
    std::fprintf(stderr,
        "usage: anygear_d4xx_spike_viewer_host "
        "--realsense-runtime PATH --worker PATH --model PATH --config PATH "
        "[--log PATH] [--port 8765] [--no-open]\n");
}

bool parse_integer(const wchar_t* value, int minimum, int maximum,
                   int* output) {
    if (!value || !*value || !output) return false;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value, &end, 10);
    if (!end || *end != L'\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *output = static_cast<int>(parsed);
    return true;
}

bool parse_arguments(int argc, wchar_t** argv, Options* output) {
    if (!output) return false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto next_path = [&](std::filesystem::path* path) {
            if (index + 1 >= argc) return false;
            *path = argv[++index];
            return true;
        };
        if (argument == L"--realsense-runtime") {
            if (!next_path(&output->realsense_runtime)) return false;
        } else if (argument == L"--worker") {
            if (!next_path(&output->worker_executable)) return false;
        } else if (argument == L"--model") {
            if (!next_path(&output->model)) return false;
        } else if (argument == L"--config") {
            if (!next_path(&output->config)) return false;
        } else if (argument == L"--log") {
            if (!next_path(&output->log)) return false;
        } else if (argument == L"--port") {
            if (index + 1 >= argc ||
                !parse_integer(argv[++index], 1024, 65535, &output->port)) {
                return false;
            }
        } else if (argument == L"--no-open") {
            output->open_browser = false;
        } else if (argument == L"--help" || argument == L"-h") {
            print_usage();
            std::exit(0);
        } else {
            std::fwprintf(stderr, L"unknown argument: %ls\n", argument.c_str());
            return false;
        }
    }
    return !output->realsense_runtime.empty() &&
        !output->worker_executable.empty() && !output->model.empty() &&
        !output->config.empty();
}

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

void log_line(void*, const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
}

template <typename T>
T json_number(const nlohmann::json& value, const char* name, T fallback,
              T minimum, T maximum) {
    const auto found = value.find(name);
    if (found == value.end()) return fallback;
    if constexpr (std::is_integral_v<T>) {
        if (!found->is_number_integer()) {
            throw std::runtime_error(std::string(name) + " must be an integer");
        }
    } else if (!found->is_number()) {
        throw std::runtime_error(std::string(name) + " must be a number");
    }
    const T result = found->get<T>();
    if (result < minimum || result > maximum) {
        throw std::runtime_error(std::string(name) + " is outside its range");
    }
    return result;
}

vp4u::D4xxOptions load_capture_options(const Options& options) {
    std::ifstream stream(options.config);
    if (!stream) throw std::runtime_error("cannot open runtime config");
    const nlohmann::json document = nlohmann::json::parse(stream);
    if (!document.is_object()) {
        throw std::runtime_error("runtime config root must be an object");
    }
    const nlohmann::json placement = document.value(
        "D4xxPlacement", nlohmann::json::object());
    if (!placement.is_object()) {
        throw std::runtime_error("D4xxPlacement must be an object");
    }

    vp4u::D4xxOptions result;
    result.runtime_path = options.realsense_runtime.wstring();
    result.width = 848;
    result.height = 480;
    result.fps = 30;
    result.primary_device = json_number<int>(
        placement, "PrimaryDevice", 0, 0, 1);
    result.required_devices = json_number<int>(
        placement, "RequiredDevices", 2, 1, 2);
    result.frame_stall_timeout_ms = json_number<int>(
        placement, "FrameStallTimeoutMs", 1500, 100, 60000);
    result.reconnect_delay_ms = json_number<int>(
        placement, "ReconnectDelayMs", 500, 50, 60000);
    if (const auto found = placement.find("PrimarySerial");
        found != placement.end()) {
        if (!found->is_string()) {
            throw std::runtime_error("PrimarySerial must be a string");
        }
        result.primary_serial = found->get<std::string>();
    }
    result.align_depth_to_infrared = false;
    result.enable_infrared = false;

    std::string emitter_mode = "all-on";
    if (const auto found = placement.find("EmitterMode");
        found != placement.end()) {
        if (!found->is_string()) {
            throw std::runtime_error("EmitterMode must be a string");
        }
        emitter_mode = found->get<std::string>();
    }
    result.emitter_enabled_by_device.assign(
        static_cast<std::size_t>(result.required_devices), 1);
    result.emitter_on_off_by_device.assign(
        static_cast<std::size_t>(result.required_devices), 0);
    if (emitter_mode == "all-off") {
        std::fill(result.emitter_enabled_by_device.begin(),
                  result.emitter_enabled_by_device.end(), 0);
    } else if (emitter_mode == "first-only") {
        for (std::size_t index = 0;
             index < result.emitter_enabled_by_device.size(); ++index) {
            result.emitter_enabled_by_device[index] = index == 0 ? 1 : 0;
        }
    } else if (emitter_mode == "second-only") {
        for (std::size_t index = 0;
             index < result.emitter_enabled_by_device.size(); ++index) {
            result.emitter_enabled_by_device[index] = index == 1 ? 1 : 0;
        }
    } else if (emitter_mode == "alternating") {
        std::fill(result.emitter_on_off_by_device.begin(),
                  result.emitter_on_off_by_device.end(), 1);
    } else if (emitter_mode != "all-on" && emitter_mode != "unchanged") {
        throw std::runtime_error("unsupported EmitterMode");
    }
    if (emitter_mode == "unchanged") {
        result.emitter_enabled_by_device.clear();
        result.emitter_on_off_by_device.clear();
    }
    // Match the production diagnostics profile selected for this room.
    result.visual_preset_by_device.assign(
        static_cast<std::size_t>(result.required_devices), 4);
    result.log = log_line;
    return result;
}

anygear::spike::DepthInput make_depth_input(
    const vp4u::D4xxDepthFrame& frame) {
    anygear::spike::DepthInput input;
    input.frame_sequence = frame.sequence;
    input.host_time_ns = frame.host_time_ns;
    input.depth_scale_m = frame.depth_scale_m;
    input.width = static_cast<std::uint32_t>(frame.intrinsics.width);
    input.height = static_cast<std::uint32_t>(frame.intrinsics.height);
    input.principal_x = frame.intrinsics.principal_x;
    input.principal_y = frame.intrinsics.principal_y;
    input.focal_x = frame.intrinsics.focal_x;
    input.focal_y = frame.intrinsics.focal_y;
    input.distortion_model = frame.intrinsics.distortion_model;
    std::copy(std::begin(frame.intrinsics.distortion),
              std::end(frame.intrinsics.distortion),
              std::begin(input.distortion));
    input.device_index = static_cast<std::uint32_t>(frame.device_index);
    input.serial = frame.serial;
    input.depth = frame.depth.data();
    input.depth_samples = frame.depth.size();
    return input;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parse_arguments(argc, argv, &options)) {
        print_usage();
        return 2;
    }
    for (const auto& path : {
             options.realsense_runtime, options.worker_executable,
             options.model, options.config}) {
        if (!regular_file(path)) {
            std::fwprintf(stderr, L"required file is missing: %ls\n",
                          path.c_str());
            return 2;
        }
    }
    SetConsoleCtrlHandler(console_control_handler, TRUE);

    try {
        const vp4u::D4xxOptions capture_options =
            load_capture_options(options);
        std::string error;
        std::shared_ptr<vp4u::D4xxSource> cameras =
            vp4u::D4xxSource::acquire(capture_options, &error);
        if (!cameras) {
            throw std::runtime_error("D4xx startup failed: " + error);
        }
        if (!cameras->wait_until_ready(10000)) {
            throw std::runtime_error("D4xx streams did not become ready");
        }

        anygear::spike::LaunchOptions launch;
        launch.executable = options.worker_executable.wstring();
        launch.arguments = {
            L"-m", L"anygear_spike.live_preview",
            L"--model", options.model.wstring(),
            L"--runtime-config", options.config.wstring(),
            L"--port", std::to_wstring(options.port),
        };
        if (!options.log.empty()) {
            launch.arguments.insert(launch.arguments.end(), {
                L"--log", options.log.wstring(),
            });
        }
        launch.startup_timeout_ms = 45000;
        launch.shutdown_timeout_ms = 3000;
        launch.hide_window = true;
        launch.log = log_line;
        std::unique_ptr<anygear::spike::IpcHost> ipc =
            anygear::spike::IpcHost::launch(launch, &error);
        if (!ipc) {
            throw std::runtime_error("preview worker failed: " + error);
        }

        const std::wstring url = L"http://127.0.0.1:" +
            std::to_wstring(options.port) + L"/";
        std::fwprintf(stderr, L"[anygear-viewer] ready: %ls\n", url.c_str());
        std::fprintf(stderr,
            "[anygear-viewer] keep this window open; press Ctrl+C to stop\n");
        std::fflush(stderr);
        if (options.open_browser) {
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }

        std::array<std::uint64_t, anygear::spike::kCameraCount> sequences{};
        while (!g_stop) {
            bool published = false;
            for (std::size_t camera = 0;
                 camera < anygear::spike::kCameraCount; ++camera) {
                vp4u::D4xxDepthFrame frame;
                if (!cameras->get_depth_frame(
                        static_cast<int>(camera), sequences[camera], &frame)) {
                    continue;
                }
                const anygear::spike::DepthInput input =
                    make_depth_input(frame);
                if (!ipc->publish_depth(camera, input, &error)) {
                    throw std::runtime_error("depth publish failed: " + error);
                }
                sequences[camera] = frame.sequence;
                published = true;
            }
            const anygear::spike::WorkerState state = ipc->worker_state();
            if (state == anygear::spike::WorkerState::faulted) {
                throw std::runtime_error(
                    "preview worker fault: " + ipc->worker_error());
            }
            if (state == anygear::spike::WorkerState::stopping ||
                state == anygear::spike::WorkerState::stopped) {
                break;
            }
            if (!published) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ipc->stop();
        std::fprintf(stderr, "[anygear-viewer] stopped\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[anygear-viewer] ERROR: %s\n", error.what());
        return 1;
    }
}
