#include "d4xx/spike_ipc_protocol.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>

int main() {
    auto shared = std::make_unique<anygear::spike::SharedMemory>();
    std::memset(shared.get(), 0, sizeof(*shared));
    shared->control.magic = anygear::spike::kProtocolMagic;
    shared->control.version = anygear::spike::kProtocolVersion;
    shared->control.mapping_bytes = sizeof(*shared);

    auto& frame = shared->cameras[0];
    anygear::spike::begin_write(&frame.sequence);
    frame.frame_sequence = 42;
    frame.width = 4;
    frame.height = 3;
    frame.focal_x = 100.0f;
    frame.focal_y = 101.0f;
    for (std::uint16_t index = 0; index < 12; ++index) {
        frame.depth[index] = static_cast<std::uint16_t>(1000 + index);
    }
    anygear::spike::end_write(&frame.sequence);

    const LONG64 sequence = InterlockedCompareExchange64(
        &frame.sequence, 0, 0);
    if (sequence == 0 || (sequence & 1) != 0 ||
        frame.frame_sequence != 42 || frame.depth[11] != 1011) {
        std::cerr << "shared depth publication failed\n";
        return 1;
    }

    auto& pose = shared->pose;
    anygear::spike::begin_write(&pose.sequence);
    pose.generation = 7;
    pose.flags = anygear::spike::pose_fused_valid |
        anygear::spike::pose_camera_0_valid;
    pose.joint_count = anygear::spike::kJointCount;
    pose.fused_joints[14][2] = 2.5f;
    pose.source_sequence[0] = 42;
    anygear::spike::end_write(&pose.sequence);
    if (pose.generation != 7 || pose.fused_joints[14][2] != 2.5f ||
        pose.source_sequence[0] != 42) {
        std::cerr << "shared pose publication failed\n";
        return 1;
    }

    std::cout << "SPiKE IPC ABI v" << anygear::spike::kProtocolVersion
              << ": " << sizeof(*shared) << " bytes\n";
    return 0;
}
