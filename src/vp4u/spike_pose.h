// SPiKE 15-joint skeleton to VP4U's 33-landmark intermediate pose.
#ifndef VP4U_SPIKE_POSE_H
#define VP4U_SPIKE_POSE_H

#include "d4xx/spike_ipc_protocol.h"
#include "pose.h"

namespace vp4u {

void map_spike_pose(const anygear::spike::PoseFrame& source,
                    PoseResult* output);

} // namespace vp4u

#endif // VP4U_SPIKE_POSE_H
