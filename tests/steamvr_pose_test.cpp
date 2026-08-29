#include "vp4u/steamvr.h"

#include <cmath>
#include <cstdio>

namespace {

using vp4u::SteamVrBodyRole;
using vp4u::SteamVrDevicePose;
using vp4u::SteamVrPoseFrame;

SteamVrDevicePose pose(float x, float y, float z) {
    SteamVrDevicePose value;
    value.tracked = true;
    value.position[0] = x;
    value.position[1] = y;
    value.position[2] = z;
    return value;
}

void set(SteamVrPoseFrame* frame, SteamVrBodyRole role,
         float x, float y, float z) {
    frame->device[static_cast<size_t>(role)] = pose(x, y, z);
}

bool near(float actual, float expected, float tolerance = 0.001f) {
    return std::fabs(actual - expected) <= tolerance;
}

int fail(const char* message) {
    std::fprintf(stderr, "steamvr pose test failed: %s\n", message);
    return 1;
}

} // namespace

int main() {
    vp4u::SteamVrBodyRole parsed_role = vp4u::SteamVrBodyRole::Count;
    if (!vp4u::steamvr_body_role_from_setting(
            "TrackerRole_LeftFoot", &parsed_role) ||
        parsed_role != vp4u::SteamVrBodyRole::LeftFoot ||
        vp4u::steamvr_body_role_from_setting(
            "TrackerRole_Camera", &parsed_role)) {
        return fail("SteamVR tracker-role strings were mapped incorrectly");
    }

    SteamVrPoseFrame frame;
    set(&frame, SteamVrBodyRole::Head, 0.f, 1.70f, 0.f);
    set(&frame, SteamVrBodyRole::LeftHand, -0.45f, 1.20f, -0.05f);
    set(&frame, SteamVrBodyRole::RightHand, 0.45f, 1.20f, -0.05f);
    set(&frame, SteamVrBodyRole::Waist, 0.f, 1.00f, 0.f);
    set(&frame, SteamVrBodyRole::LeftFoot, -0.14f, 0.05f, 0.f);
    set(&frame, SteamVrBodyRole::RightFoot, 0.14f, 0.05f, 0.f);
    set(&frame, SteamVrBodyRole::LeftKnee, -0.14f, 0.55f, 0.04f);

    vp4u::SteamVrTrackingOptions options;
    vp4u::SteamVrCalibration calibration;
    vp4u::PoseResult result;
    if (!vp4u::map_steamvr_pose(frame, options, &calibration, &result)) {
        return fail("mapper returned an API failure");
    }
    if (!result.valid || !calibration.ready) {
        return fail("six-point frame was not accepted");
    }

    const float hip_x = (result.world[vp4u::MP_LeftHip][0] +
                         result.world[vp4u::MP_RightHip][0]) * 0.5f;
    const float hip_y = (result.world[vp4u::MP_LeftHip][1] +
                         result.world[vp4u::MP_RightHip][1]) * 0.5f;
    if (!near(hip_x, options.stage_hip_x) ||
        !near(hip_y, options.stage_hip_y - 0.055f)) {
        return fail("auto-center did not place the hip at the stage anchor");
    }
    if (!(result.world[vp4u::MP_LeftShoulder][0] >
          result.world[vp4u::MP_RightShoulder][0])) {
        return fail("face-to-face yaw did not rotate the shoulder basis");
    }
    if (!(result.world[vp4u::MP_LeftFootIndex][1] <
          result.world[vp4u::MP_LeftAnkle][1])) {
        return fail("foot synthesis did not keep toes below ankles");
    }
    if (!near(result.vis[vp4u::MP_LeftKnee], 1.f) ||
        !near(result.vis[vp4u::MP_RightKnee], 0.5f)) {
        return fail("optional knee role did not replace only its inferred joint");
    }

    // Calibration is fixed: room translation must survive subsequent frames.
    for (SteamVrDevicePose& device : frame.device) {
        if (device.tracked) device.position[0] += 0.20f;
    }
    vp4u::PoseResult moved;
    if (!vp4u::map_steamvr_pose(frame, options, &calibration, &moved) ||
        !moved.valid) {
        return fail("translated frame was not accepted");
    }
    const float moved_hip_x = (moved.world[vp4u::MP_LeftHip][0] +
                               moved.world[vp4u::MP_RightHip][0]) * 0.5f;
    if (!near(moved_hip_x, hip_x - 0.20f)) {
        return fail("fixed calibration erased or changed room translation");
    }

    // Strict mode must not report full-body tracking when a required foot is
    // absent. The explicit evaluation mode may infer it at floor height.
    frame.device[static_cast<size_t>(SteamVrBodyRole::LeftFoot)].tracked = false;
    vp4u::PoseResult missing;
    if (!vp4u::map_steamvr_pose(frame, options, &calibration, &missing) ||
        missing.valid) {
        return fail("strict mode accepted a missing foot tracker");
    }
    options.require_feet = false;
    options.require_waist = false;
    frame.device[static_cast<size_t>(SteamVrBodyRole::RightFoot)].tracked = false;
    frame.device[static_cast<size_t>(SteamVrBodyRole::Waist)].tracked = false;
    frame.device[static_cast<size_t>(SteamVrBodyRole::LeftKnee)].tracked = false;
    if (!vp4u::map_steamvr_pose(frame, options, &calibration, &missing) ||
        !missing.valid || missing.vis[vp4u::MP_LeftFootIndex] > 0.5f ||
        missing.vis[vp4u::MP_LeftHip] > 0.5f ||
        !near(missing.confidence, 0.5f)) {
        return fail("explicit three-point mode was not marked inferred");
    }

    std::puts("steamvr pose mapping: ok");
    return 0;
}
