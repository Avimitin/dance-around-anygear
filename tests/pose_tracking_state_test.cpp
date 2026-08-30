#include "vp4u/pose.h"

#include <cmath>
#include <iostream>

namespace {

bool near(float first, float second) {
    return std::fabs(first - second) < 1e-6f;
}

} // namespace

int main() {
    vp4u::PoseResult pose;

    pose.vis[vp4u::MP_LeftWrist] = 0.5f;
    if (vp4u::pose_tracking_state(pose, vp4u::MP_LeftWrist) != 1) {
        std::cerr << "legacy visibility threshold changed\n";
        return 1;
    }
    pose.vis[vp4u::MP_LeftWrist] = 0.5001f;
    if (vp4u::pose_tracking_state(pose, vp4u::MP_LeftWrist) != 2) {
        std::cerr << "legacy tracked-state mapping changed\n";
        return 1;
    }

    pose.has_tracking_state = true;
    pose.vis[vp4u::MP_LeftWrist] = 1.0f;
    for (int state = 0; state <= 2; ++state) {
        pose.tracking_state[vp4u::MP_LeftWrist] =
            static_cast<std::uint8_t>(state);
        if (vp4u::pose_tracking_state(pose, vp4u::MP_LeftWrist) != state) {
            std::cerr << "explicit tracking state was not preserved\n";
            return 1;
        }
    }

    pose.tracking_state[vp4u::MP_LeftWrist] = 255;
    if (vp4u::pose_tracking_state(pose, vp4u::MP_LeftWrist) != 2) {
        std::cerr << "malformed tracking state was not clamped\n";
        return 1;
    }

    float previous[3]{};
    bool have_previous = false;
    pose.tracking_state[vp4u::MP_LeftWrist] = 2;
    pose.world[vp4u::MP_LeftWrist][0] = 1.0f;
    pose.world[vp4u::MP_LeftWrist][1] = 2.0f;
    pose.world[vp4u::MP_LeftWrist][2] = 3.0f;
    vp4u::smooth_pose_landmark(
        &pose, vp4u::MP_LeftWrist, 0.5f, previous, &have_previous);
    if (!have_previous || !near(previous[0], 1.0f) ||
        !near(previous[1], 2.0f) || !near(previous[2], 3.0f)) {
        std::cerr << "first tracked coordinate was not retained\n";
        return 1;
    }

    pose.tracking_state[vp4u::MP_LeftWrist] = 0;
    pose.world[vp4u::MP_LeftWrist][0] = 0.0f;
    pose.world[vp4u::MP_LeftWrist][1] = 0.0f;
    pose.world[vp4u::MP_LeftWrist][2] = 0.0f;
    vp4u::smooth_pose_landmark(
        &pose, vp4u::MP_LeftWrist, 0.5f, previous, &have_previous);
    if (!near(pose.world[vp4u::MP_LeftWrist][0], 1.0f) ||
        !near(pose.world[vp4u::MP_LeftWrist][1], 2.0f) ||
        !near(pose.world[vp4u::MP_LeftWrist][2], 3.0f) ||
        !near(previous[0], 1.0f)) {
        std::cerr << "untracked coordinate contaminated smoothing history\n";
        return 1;
    }

    pose.tracking_state[vp4u::MP_LeftWrist] = 2;
    pose.world[vp4u::MP_LeftWrist][0] = 3.0f;
    pose.world[vp4u::MP_LeftWrist][1] = 4.0f;
    pose.world[vp4u::MP_LeftWrist][2] = 5.0f;
    vp4u::smooth_pose_landmark(
        &pose, vp4u::MP_LeftWrist, 0.5f, previous, &have_previous);
    if (!near(pose.world[vp4u::MP_LeftWrist][0], 2.0f) ||
        !near(pose.world[vp4u::MP_LeftWrist][1], 3.0f) ||
        !near(pose.world[vp4u::MP_LeftWrist][2], 4.0f)) {
        std::cerr << "reacquired coordinate used a missing-frame placeholder\n";
        return 1;
    }

    std::cout << "Pose tracking-state mapping and smoothing passed\n";
    return 0;
}
