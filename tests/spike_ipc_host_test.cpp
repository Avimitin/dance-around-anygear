#include "d4xx/spike_ipc.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argument_count, char** arguments) {
    if (argument_count != 2 && argument_count != 4) {
        std::cerr << "usage: anygear_spike_ipc_host_test <python.exe> "
                     "[--exit-without-cleanup <pid-file>]\n";
        return 2;
    }
    const bool exit_without_cleanup = argument_count == 4 &&
        std::string(arguments[2]) == "--exit-without-cleanup";
    if (argument_count == 4 && !exit_without_cleanup) return 2;
    anygear::spike::LaunchOptions options;
    options.executable = std::filesystem::path(arguments[1]).wstring();
    options.arguments = {L"-m", L"anygear_spike.mock_worker"};
    options.startup_timeout_ms = 10000;
    options.shutdown_timeout_ms = 1000;
    options.hide_window = false;
    std::string error;
    auto host = anygear::spike::IpcHost::launch(options, &error);
    if (!host) {
        std::cerr << "launch failed: " << error << "\n";
        return 1;
    }
    if (exit_without_cleanup) {
        std::ofstream pid_file(arguments[3], std::ios::trunc);
        pid_file << host->worker_process_id() << "\n";
        pid_file.flush();
        if (!pid_file) return 1;
        ExitProcess(0);
    }

    std::array<std::uint16_t, 12> depth{};
    for (std::size_t index = 0; index < depth.size(); ++index) {
        depth[index] = static_cast<std::uint16_t>(1000 + index);
    }
    for (std::size_t camera = 0; camera < 2; ++camera) {
        anygear::spike::DepthInput input;
        input.frame_sequence = camera + 10;
        input.host_time_ns = static_cast<std::int64_t>(camera + 1) * 1000;
        input.width = 4;
        input.height = 3;
        input.principal_x = 1.5f;
        input.principal_y = 1.0f;
        input.focal_x = 100.0f;
        input.focal_y = 101.0f;
        input.device_index = camera;
        input.serial = camera == 0 ? "camera-a" : "camera-b";
        input.depth = depth.data();
        input.depth_samples = depth.size();
        if (!host->publish_depth(camera, input, &error)) {
            std::cerr << "publish failed: " << error << "\n";
            return 1;
        }
    }

    anygear::spike::PoseFrame pose{};
    if (!host->wait_for_pose(0, 5000, &pose)) {
        std::cerr << "pose timed out, worker state="
                  << static_cast<int>(host->worker_state())
                  << " error=" << host->worker_error() << "\n";
        return 1;
    }
    if (pose.generation != 1 || pose.flags != 0x7 ||
        pose.source_sequence[0] != 10 || pose.source_sequence[1] != 11 ||
        std::fabs(pose.fused_joints[14][0] - 14.0f) > 1e-6f ||
        std::fabs(pose.fused_joints[0][1] - 1000.0f) > 1e-6f) {
        std::cerr << "cross-language pose payload mismatch\n";
        return 1;
    }
    std::cout << "SPiKE IPC host/worker round trip passed (pid="
              << host->worker_process_id() << ")\n";
    return 0;
}
