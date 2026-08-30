#include "spike_ipc.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace anygear::spike {
namespace {

std::string windows_error(const char* operation, DWORD code) {
    return std::string(operation) + " failed (Win32 " +
        std::to_string(code) + ")";
}

std::wstring quote_argument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result;
    result.push_back(L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

LONG64 read_sequence(volatile LONG64* value) {
    return InterlockedCompareExchange64(value, 0, 0);
}

std::wstring make_object_suffix() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::wostringstream stream;
    stream << GetCurrentProcessId() << L"_" <<
        static_cast<unsigned long long>(counter.QuadPart);
    return stream.str();
}

bool process_is_running(HANDLE process) {
    if (!process) return false;
    DWORD exit_code = 0;
    return GetExitCodeProcess(process, &exit_code) &&
        exit_code == STILL_ACTIVE;
}

} // namespace

std::unique_ptr<IpcHost> IpcHost::launch(
    const LaunchOptions& options, std::string* error) {
    auto result = std::unique_ptr<IpcHost>(new IpcHost());
    if (!result->initialize(options, error)) return {};
    return result;
}

IpcHost::~IpcHost() {
    stop();
    close_handles();
}

void IpcHost::log(const char* format, ...) const {
    char message[768];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    if (options_.log) {
        options_.log(options_.log_context, message);
    } else {
        std::fprintf(stderr, "[anygear-spike-ipc] %s\n", message);
        std::fflush(stderr);
    }
}

bool IpcHost::initialize(const LaunchOptions& options, std::string* error) {
    options_ = options;
    if (options_.executable.empty()) {
        if (error) *error = "SPiKE worker executable path is empty";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(options_.executable.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (error) *error = "SPiKE worker executable does not exist";
        return false;
    }

    const std::wstring suffix = make_object_suffix();
    mapping_name_ = L"Local\\DanceAroundAnygearSpikeMap_" + suffix;
    input_event_name_ = L"Local\\DanceAroundAnygearSpikeInput_" + suffix;
    output_event_name_ = L"Local\\DanceAroundAnygearSpikeOutput_" + suffix;

    static_assert(sizeof(SharedMemory) <=
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedMemory)), mapping_name_.c_str());
    if (!mapping_) {
        if (error) *error = windows_error("CreateFileMappingW", GetLastError());
        close_handles();
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (error) *error = "SPiKE shared-memory name collision";
        close_handles();
        return false;
    }
    shared_ = static_cast<SharedMemory*>(MapViewOfFile(
        mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemory)));
    if (!shared_) {
        if (error) *error = windows_error("MapViewOfFile", GetLastError());
        close_handles();
        return false;
    }
    input_event_ = CreateEventW(
        nullptr, FALSE, FALSE, input_event_name_.c_str());
    output_event_ = CreateEventW(
        nullptr, FALSE, FALSE, output_event_name_.c_str());
    if (!input_event_ || !output_event_) {
        if (error) *error = windows_error("CreateEventW", GetLastError());
        close_handles();
        return false;
    }

    std::memset(shared_, 0, sizeof(*shared_));
    shared_->control.magic = kProtocolMagic;
    shared_->control.version = kProtocolVersion;
    shared_->control.header_bytes = sizeof(ControlBlock);
    shared_->control.mapping_bytes = sizeof(SharedMemory);
    shared_->control.camera_count = kCameraCount;
    shared_->control.maximum_depth_width = kMaximumDepthWidth;
    shared_->control.maximum_depth_height = kMaximumDepthHeight;
    shared_->control.maximum_depth_samples = kMaximumDepthSamples;
    shared_->control.joint_count = kJointCount;
    shared_->control.host_process_id = GetCurrentProcessId();
    InterlockedExchange(
        &shared_->control.worker_state,
        static_cast<LONG>(WorkerState::starting));

    std::vector<std::wstring> arguments = options_.arguments;
    arguments.insert(arguments.end(), {
        L"--ipc-mapping", mapping_name_,
        L"--ipc-input-event", input_event_name_,
        L"--ipc-output-event", output_event_name_,
        L"--ipc-parent-pid", std::to_wstring(GetCurrentProcessId()),
    });
    std::wstring command_line = quote_argument(options_.executable);
    for (const std::wstring& argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_argument(argument);
    }
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                job_, JobObjectExtendedLimitInformation,
                &limits, sizeof limits)) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const DWORD creation_flags = CREATE_SUSPENDED |
        BELOW_NORMAL_PRIORITY_CLASS |
        (options_.hide_window ? CREATE_NO_WINDOW : 0);
    if (!CreateProcessW(
            options_.executable.c_str(), mutable_command.data(), nullptr,
            nullptr, FALSE, creation_flags, nullptr, nullptr,
            &startup, &process)) {
        if (error) *error = windows_error("CreateProcessW", GetLastError());
        close_handles();
        return false;
    }
    process_ = process.hProcess;
    shared_->control.worker_process_id = process.dwProcessId;
    if (job_ && !AssignProcessToJobObject(job_, process_)) {
        log("worker could not join cleanup job (Win32 %lu)", GetLastError());
        CloseHandle(job_);
        job_ = nullptr;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    const ULONGLONG deadline =
        GetTickCount64() + options_.startup_timeout_ms;
    while (GetTickCount64() < deadline) {
        const WorkerState state = worker_state();
        if (state == WorkerState::ready || state == WorkerState::running) {
            const DWORD reported_worker = worker_process_id();
            log("worker ready (launcher pid=%lu, worker pid=%lu, "
                "shared-memory=%zu bytes)", process.dwProcessId,
                reported_worker, sizeof(SharedMemory));
            return true;
        }
        if (state == WorkerState::faulted) {
            if (error) {
                *error = worker_error();
                if (error->empty()) *error = "SPiKE worker reported a fault";
            }
            close_handles();
            return false;
        }
        if (!process_is_running(process_)) {
            DWORD exit_code = 0;
            GetExitCodeProcess(process_, &exit_code);
            if (error) {
                *error = "SPiKE worker exited during startup (code " +
                    std::to_string(exit_code) + ")";
            }
            close_handles();
            return false;
        }
        WaitForSingleObject(output_event_, 50);
    }
    if (error) *error = "SPiKE worker startup timed out";
    close_handles();
    return false;
}

bool IpcHost::publish_depth(std::size_t camera_index,
                            const DepthInput& input,
                            std::string* error) {
    if (!shared_ || camera_index >= kCameraCount) {
        if (error) *error = "invalid SPiKE camera index";
        return false;
    }
    const std::size_t expected =
        static_cast<std::size_t>(input.width) * input.height;
    if (!input.depth || input.width == 0 || input.height == 0 ||
        input.width > kMaximumDepthWidth ||
        input.height > kMaximumDepthHeight ||
        input.depth_samples != expected ||
        expected > kMaximumDepthSamples ||
        input.focal_x <= 0.0f || input.focal_y <= 0.0f) {
        if (error) *error = "invalid SPiKE depth frame";
        return false;
    }

    DepthFrame& output = shared_->cameras[camera_index];
    begin_write(&output.sequence);
    output.frame_sequence = input.frame_sequence;
    output.host_time_ns = input.host_time_ns;
    output.depth_scale_m = input.depth_scale_m;
    output.width = input.width;
    output.height = input.height;
    output.principal_x = input.principal_x;
    output.principal_y = input.principal_y;
    output.focal_x = input.focal_x;
    output.focal_y = input.focal_y;
    output.distortion_model = input.distortion_model;
    std::copy(std::begin(input.distortion), std::end(input.distortion),
              std::begin(output.distortion));
    output.device_index = input.device_index;
    std::memset(output.serial, 0, sizeof output.serial);
    const std::size_t serial_length = std::min(
        input.serial.size(), sizeof output.serial - 1);
    std::memcpy(output.serial, input.serial.data(), serial_length);
    std::memcpy(output.depth, input.depth,
                expected * sizeof(std::uint16_t));
    end_write(&output.sequence);
    SetEvent(input_event_);
    return true;
}

bool IpcHost::wait_for_pose(std::uint64_t after_generation,
                            std::uint32_t timeout_ms,
                            PoseFrame* output) {
    if (!shared_ || !output) return false;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const LONG64 first = read_sequence(&shared_->pose.sequence);
            if ((first & 1) != 0) {
                SwitchToThread();
                continue;
            }
            MemoryBarrier();
            std::memcpy(output, &shared_->pose, sizeof(*output));
            MemoryBarrier();
            const LONG64 second = read_sequence(&shared_->pose.sequence);
            if (first == second && (second & 1) == 0 &&
                output->generation > after_generation) {
                return true;
            }
        }
        if (timeout_ms == 0 || GetTickCount64() >= deadline) return false;
        const ULONGLONG remaining = deadline - GetTickCount64();
        WaitForSingleObject(
            output_event_, static_cast<DWORD>(std::min<ULONGLONG>(remaining, 25)));
    }
}

WorkerState IpcHost::worker_state() const {
    if (!shared_) return WorkerState::empty;
    const LONG value = InterlockedCompareExchange(
        &shared_->control.worker_state, 0, 0);
    return static_cast<WorkerState>(value);
}

std::string IpcHost::worker_error() const {
    if (!shared_) return {};
    MemoryBarrier();
    const void* terminator = std::memchr(
        shared_->control.error_message, 0,
        sizeof shared_->control.error_message);
    const std::size_t length = terminator
        ? static_cast<const char*>(terminator) -
            shared_->control.error_message
        : sizeof shared_->control.error_message;
    return std::string(shared_->control.error_message, length);
}

DWORD IpcHost::worker_process_id() const {
    return shared_ ? shared_->control.worker_process_id : 0;
}

const std::wstring& IpcHost::mapping_name() const { return mapping_name_; }
const std::wstring& IpcHost::input_event_name() const {
    return input_event_name_;
}
const std::wstring& IpcHost::output_event_name() const {
    return output_event_name_;
}

void IpcHost::stop() {
    if (stopping_) return;
    stopping_ = true;
    if (shared_) {
        InterlockedExchange(&shared_->control.shutdown_requested, 1);
        SetEvent(input_event_);
    }
    if (process_ && process_is_running(process_)) {
        const DWORD result = WaitForSingleObject(
            process_, options_.shutdown_timeout_ms);
        if (result == WAIT_TIMEOUT) {
            log("worker did not stop within %lu ms; closing its cleanup job",
                options_.shutdown_timeout_ms);
            if (job_) {
                CloseHandle(job_);
                job_ = nullptr;
            } else {
                TerminateProcess(process_, ERROR_PROCESS_ABORTED);
            }
            WaitForSingleObject(process_, 1000);
        }
    }
}

void IpcHost::close_handles() {
    if (shared_) {
        UnmapViewOfFile(shared_);
        shared_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    if (output_event_) {
        CloseHandle(output_event_);
        output_event_ = nullptr;
    }
    if (input_event_) {
        CloseHandle(input_event_);
        input_event_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

} // namespace anygear::spike
