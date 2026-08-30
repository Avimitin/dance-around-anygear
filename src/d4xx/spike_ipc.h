// Host-side owner of the SPiKE worker shared-memory channel.
#ifndef ANYGEAR_D4XX_SPIKE_IPC_H
#define ANYGEAR_D4XX_SPIKE_IPC_H

#include "spike_ipc_protocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace anygear::spike {

using LogCallback = void (*)(void* context, const char* message);

struct DepthInput {
    std::uint64_t frame_sequence = 0;
    std::int64_t host_time_ns = 0;
    float depth_scale_m = 0.001f;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float principal_x = 0.0f;
    float principal_y = 0.0f;
    float focal_x = 0.0f;
    float focal_y = 0.0f;
    std::int32_t distortion_model = 0;
    float distortion[5]{};
    std::uint32_t device_index = 0;
    std::string serial;
    const std::uint16_t* depth = nullptr;
    std::size_t depth_samples = 0;
};

struct LaunchOptions {
    std::wstring executable;
    std::vector<std::wstring> arguments;
    std::uint32_t startup_timeout_ms = 45000;
    std::uint32_t shutdown_timeout_ms = 3000;
    bool hide_window = true;
    LogCallback log = nullptr;
    void* log_context = nullptr;
};

class IpcHost {
public:
    static std::unique_ptr<IpcHost> launch(
        const LaunchOptions& options, std::string* error = nullptr);

    ~IpcHost();
    IpcHost(const IpcHost&) = delete;
    IpcHost& operator=(const IpcHost&) = delete;

    bool publish_depth(std::size_t camera_index, const DepthInput& input,
                       std::string* error = nullptr);
    bool wait_for_pose(std::uint64_t after_generation,
                       std::uint32_t timeout_ms, PoseFrame* output);

    WorkerState worker_state() const;
    std::string worker_error() const;
    DWORD worker_process_id() const;
    const std::wstring& mapping_name() const;
    const std::wstring& input_event_name() const;
    const std::wstring& output_event_name() const;

    void stop();

private:
    IpcHost() = default;
    bool initialize(const LaunchOptions& options, std::string* error);
    void close_handles();
    void log(const char* format, ...) const;

    LaunchOptions options_;
    std::wstring mapping_name_;
    std::wstring input_event_name_;
    std::wstring output_event_name_;
    HANDLE mapping_ = nullptr;
    HANDLE input_event_ = nullptr;
    HANDLE output_event_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    SharedMemory* shared_ = nullptr;
    bool stopping_ = false;
};

} // namespace anygear::spike

#endif // ANYGEAR_D4XX_SPIKE_IPC_H
