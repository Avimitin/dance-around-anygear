// Shared-memory ABI between the D4xx entry DLL and the SPiKE worker.
#ifndef ANYGEAR_D4XX_SPIKE_IPC_PROTOCOL_H
#define ANYGEAR_D4XX_SPIKE_IPC_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <windows.h>

namespace anygear::spike {

constexpr std::uint32_t kProtocolMagic = 0x4b505344U; // "DSPK"
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kCameraCount = 2;
constexpr std::size_t kJointCount = 15;
constexpr std::size_t kMaximumDepthWidth = 848;
constexpr std::size_t kMaximumDepthHeight = 480;
constexpr std::size_t kMaximumDepthSamples =
    kMaximumDepthWidth * kMaximumDepthHeight;

enum class WorkerState : LONG {
    empty = 0,
    starting = 1,
    loading_model = 2,
    ready = 3,
    running = 4,
    faulted = 5,
    stopping = 6,
    stopped = 7,
};

enum PoseFlags : std::uint32_t {
    pose_fused_valid = 1U << 0,
    pose_camera_0_valid = 1U << 1,
    pose_camera_1_valid = 1U << 2,
};

// All shared structures have fixed-width fields and explicit trailing padding.
// A writer publishes a frame by changing sequence to odd, filling the payload,
// issuing a full memory barrier, and changing sequence to the next even value.
// A reader accepts a snapshot only when equal even sequence values surround the
// copy. No process-shared C++ object lifetime is assumed.
struct alignas(64) ControlBlock {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t header_bytes;
    std::uint32_t mapping_bytes;
    std::uint32_t camera_count;
    std::uint32_t maximum_depth_width;
    std::uint32_t maximum_depth_height;
    std::uint32_t maximum_depth_samples;
    std::uint32_t joint_count;
    volatile LONG worker_state;
    std::uint32_t host_process_id;
    std::uint32_t worker_process_id;
    volatile LONG shutdown_requested;
    volatile LONG64 worker_heartbeat_ms;
    char error_message[160];
    std::uint8_t reserved[40];
};

struct alignas(64) DepthFrame {
    volatile LONG64 sequence;
    std::uint64_t frame_sequence;
    std::int64_t host_time_ns;
    float depth_scale_m;
    std::uint32_t width;
    std::uint32_t height;
    float principal_x;
    float principal_y;
    float focal_x;
    float focal_y;
    std::int32_t distortion_model;
    float distortion[5];
    std::uint32_t device_index;
    char serial[32];
    std::uint8_t reserved[16];
    std::uint16_t depth[kMaximumDepthSamples];
};

struct alignas(64) PoseFrame {
    volatile LONG64 sequence;
    std::uint64_t generation;
    std::int64_t source_time_ns;
    std::uint32_t worker_time_us;
    std::uint32_t flags;
    std::uint32_t joint_count;
    std::uint32_t reserved_0;
    float fused_joints[kJointCount][3];
    float fused_confidence[kJointCount];
    std::uint8_t fused_tracking_state[kJointCount];
    std::uint8_t reserved_1[25];
    float camera_joints[kCameraCount][kJointCount][3];
    float camera_confidence[kCameraCount][kJointCount];
    float camera_centroid[kCameraCount][3];
    std::uint64_t source_sequence[kCameraCount];
    std::uint8_t reserved_2[56];
};

struct alignas(64) SharedMemory {
    ControlBlock control;
    DepthFrame cameras[kCameraCount];
    PoseFrame pose;
};

static_assert(std::is_standard_layout_v<ControlBlock>);
static_assert(std::is_standard_layout_v<DepthFrame>);
static_assert(std::is_standard_layout_v<PoseFrame>);
static_assert(std::is_standard_layout_v<SharedMemory>);
static_assert(sizeof(ControlBlock) == 256);
static_assert(offsetof(ControlBlock, worker_heartbeat_ms) == 48);
static_assert(offsetof(ControlBlock, error_message) == 56);
static_assert(offsetof(DepthFrame, depth) == 128);
static_assert(sizeof(DepthFrame) == 814208);
static_assert(offsetof(PoseFrame, fused_joints) == 40);
static_assert(offsetof(PoseFrame, camera_joints) == 320);
static_assert(offsetof(PoseFrame, source_sequence) == 824);
static_assert(sizeof(PoseFrame) == 896);
static_assert(offsetof(SharedMemory, cameras) == 256);
static_assert(offsetof(SharedMemory, pose) == 1628672);
static_assert(sizeof(SharedMemory) == 1629568);

inline LONG64 begin_write(volatile LONG64* sequence) {
    LONG64 value = InterlockedIncrement64(sequence);
    if ((value & 1) == 0) value = InterlockedIncrement64(sequence);
    return value;
}

inline void end_write(volatile LONG64* sequence) {
    MemoryBarrier();
    LONG64 value = InterlockedIncrement64(sequence);
    if ((value & 1) != 0) InterlockedIncrement64(sequence);
}

} // namespace anygear::spike

#endif // ANYGEAR_D4XX_SPIKE_IPC_PROTOCOL_H
