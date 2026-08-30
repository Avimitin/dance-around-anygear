#include "d4xx/spike_ipc_protocol.h"
#include "vp4u/pose.h"
#include "vp4u/spike_pose.h"

#include <cmath>
#include <cstdio>

namespace {

constexpr int kHead = 0;
constexpr int kNeck = 1;
constexpr int kRightShoulder = 2;
constexpr int kLeftShoulder = 3;
constexpr int kRightElbow = 4;
constexpr int kLeftElbow = 5;
constexpr int kRightHand = 6;
constexpr int kLeftHand = 7;
constexpr int kTorso = 8;
constexpr int kRightHip = 9;
constexpr int kLeftHip = 10;
constexpr int kRightKnee = 11;
constexpr int kLeftKnee = 12;
constexpr int kRightFoot = 13;
constexpr int kLeftFoot = 14;

void set(anygear::spike::PoseFrame* frame, int joint,
         float x, float y, float z) {
    frame->fused_joints[joint][0] = x;
    frame->fused_joints[joint][1] = y;
    frame->fused_joints[joint][2] = z;
    frame->fused_confidence[joint] = 0.9f;
    frame->fused_tracking_state[joint] = 2;
}

float distance(const vp4u::PoseResult& pose, int a, int b) {
    float squared = 0.0f;
    for (int component = 0; component < 3; ++component) {
        const float delta = pose.world[a][component] -
                            pose.world[b][component];
        squared += delta * delta;
    }
    return std::sqrt(squared);
}

float axis_dot(const vp4u::PoseResult& pose, int origin,
               int first, int second) {
    float a[3]{};
    float b[3]{};
    float a_length = 0.0f;
    float b_length = 0.0f;
    for (int component = 0; component < 3; ++component) {
        a[component] = pose.world[first][component] -
                       pose.world[origin][component];
        b[component] = pose.world[second][component] -
                       pose.world[origin][component];
        a_length += a[component] * a[component];
        b_length += b[component] * b[component];
    }
    float result = 0.0f;
    for (int component = 0; component < 3; ++component) {
        result += a[component] * b[component];
    }
    return result / std::sqrt(a_length * b_length);
}

bool nearly_equal(float actual, float expected,
                  float tolerance = 0.001f) {
    return std::fabs(actual - expected) <= tolerance;
}

int fail(const char* message) {
    std::fprintf(stderr, "SPiKE pose test failed: %s\n", message);
    return 1;
}

} // namespace

int main() {
    anygear::spike::PoseFrame source{};
    source.flags = anygear::spike::pose_fused_valid;
    set(&source, kHead, 0.00f, 1.72f, 2.00f);
    set(&source, kNeck, 0.00f, 1.52f, 2.00f);
    set(&source, kRightShoulder, 0.22f, 1.45f, 2.00f);
    set(&source, kLeftShoulder, -0.22f, 1.45f, 2.00f);
    set(&source, kRightElbow, 0.45f, 1.22f, 2.00f);
    set(&source, kLeftElbow, -0.45f, 1.22f, 2.00f);
    set(&source, kRightHand, 0.70f, 1.05f, 1.98f);
    set(&source, kLeftHand, -0.70f, 1.05f, 1.98f);
    set(&source, kTorso, 0.00f, 1.18f, 2.00f);
    set(&source, kRightHip, 0.13f, 0.94f, 2.00f);
    set(&source, kLeftHip, -0.13f, 0.94f, 2.00f);
    set(&source, kRightKnee, 0.14f, 0.50f, 2.02f);
    set(&source, kLeftKnee, -0.14f, 0.50f, 2.02f);
    set(&source, kRightFoot, 0.15f, 0.07f, 2.04f);
    set(&source, kLeftFoot, -0.15f, 0.07f, 2.04f);
    source.fused_tracking_state[kLeftElbow] = 1;

    vp4u::PoseResult pose;
    vp4u::map_spike_pose(source, &pose);
    if (!pose.valid || !pose.has_tracking_state) {
        return fail("valid fused pose was rejected");
    }
    if (!nearly_equal(pose.world[vp4u::MP_LeftShoulder][0], -0.22f) ||
        !nearly_equal(pose.world[vp4u::MP_RightHip][0], 0.13f)) {
        return fail("direct model joints were not preserved");
    }
    if (pose.tracking_state[vp4u::MP_LeftWrist] != 1 ||
        pose.tracking_state[vp4u::MP_LeftPinky] != 2) {
        return fail("synthesized wrist tracking state was not conservative");
    }

    const int non_degenerate_pairs[][2] = {
        {vp4u::MP_LeftEar, vp4u::MP_RightEar},
        {vp4u::MP_LeftEye, vp4u::MP_RightEye},
        {vp4u::MP_Nose, vp4u::MP_LeftEar},
        {vp4u::MP_LeftWrist, vp4u::MP_LeftPinky},
        {vp4u::MP_LeftPinky, vp4u::MP_LeftIndex},
        {vp4u::MP_LeftPinky, vp4u::MP_LeftThumb},
        {vp4u::MP_RightWrist, vp4u::MP_RightPinky},
        {vp4u::MP_RightPinky, vp4u::MP_RightIndex},
        {vp4u::MP_RightPinky, vp4u::MP_RightThumb},
        {vp4u::MP_LeftAnkle, vp4u::MP_LeftHeel},
        {vp4u::MP_LeftAnkle, vp4u::MP_LeftFootIndex},
        {vp4u::MP_RightAnkle, vp4u::MP_RightHeel},
        {vp4u::MP_RightAnkle, vp4u::MP_RightFootIndex},
    };
    for (const auto& pair : non_degenerate_pairs) {
        if (distance(pose, pair[0], pair[1]) < 0.010f) {
            return fail("synthesized landmark geometry contains a zero bone");
        }
    }
    for (const int wrist : {vp4u::MP_LeftWrist, vp4u::MP_RightWrist}) {
        const bool left = wrist == vp4u::MP_LeftWrist;
        const int hand = left ? vp4u::MP_LeftPinky : vp4u::MP_RightPinky;
        const int thumb = left ? vp4u::MP_LeftThumb : vp4u::MP_RightThumb;
        if (std::fabs(axis_dot(pose, wrist, hand, thumb)) > 0.75f) {
            return fail("synthesized hand axes are nearly singular");
        }
    }
    for (const bool left : {true, false}) {
        const int knee = left ? vp4u::MP_LeftKnee : vp4u::MP_RightKnee;
        const int ankle = left ? vp4u::MP_LeftAnkle : vp4u::MP_RightAnkle;
        const int toe = left ? vp4u::MP_LeftFootIndex : vp4u::MP_RightFootIndex;
        if (std::fabs(axis_dot(pose, ankle, knee, toe)) > 0.60f) {
            return fail("synthesized foot direction follows the lower leg");
        }
    }
    for (int landmark = 0; landmark < vp4u::kMpLandmarkCount; ++landmark) {
        for (int component = 0; component < 3; ++component) {
            if (!std::isfinite(pose.world[landmark][component])) {
                return fail("synthesized landmark is not finite");
            }
        }
    }


    // In a T-pose, body-right is parallel to both forearms. The old lateral
    // thumb fallback therefore collapsed onto wrist->hand. Both neutral thumbs
    // must instead retain an upward component and a usable secondary IK axis.
    set(&source, kRightElbow, 0.48f, 1.45f, 2.00f);
    set(&source, kLeftElbow, -0.48f, 1.45f, 2.00f);
    set(&source, kRightHand, 0.75f, 1.45f, 2.00f);
    set(&source, kLeftHand, -0.75f, 1.45f, 2.00f);
    vp4u::map_spike_pose(source, &pose);
    if (pose.world[vp4u::MP_LeftThumb][1] <=
            pose.world[vp4u::MP_LeftWrist][1] + 0.025f ||
        pose.world[vp4u::MP_RightThumb][1] <=
            pose.world[vp4u::MP_RightWrist][1] + 0.025f) {
        return fail("T-pose neutral thumb axes did not remain anatomical");
    }

    source.flags = 0;
    vp4u::map_spike_pose(source, &pose);
    if (pose.valid || pose.has_tracking_state) {
        return fail("invalid worker frame produced a tracked pose");
    }

    std::puts("SPiKE pose mapping: ok");
    return 0;
}
