#include "spike_pose.h"

#include <algorithm>
#include <cmath>

namespace vp4u {
namespace {

enum SpikeJoint {
    spike_head = 0,
    spike_neck = 1,
    spike_right_shoulder = 2,
    spike_left_shoulder = 3,
    spike_right_elbow = 4,
    spike_left_elbow = 5,
    spike_right_hand = 6,
    spike_left_hand = 7,
    spike_torso = 8,
    spike_right_hip = 9,
    spike_left_hip = 10,
    spike_right_knee = 11,
    spike_left_knee = 12,
    spike_right_foot = 13,
    spike_left_foot = 14,
};

struct Vec3 {
    float x;
    float y;
    float z;
};

Vec3 add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 mul(Vec3 value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float length(Vec3 value) {
    return std::sqrt(dot(value, value));
}

Vec3 normalized_or(Vec3 value, Vec3 fallback) {
    const float magnitude = length(value);
    return magnitude > 1.0e-5f ? mul(value, 1.0f / magnitude) : fallback;
}

Vec3 joint_position(const anygear::spike::PoseFrame& source, int joint) {
    return {source.fused_joints[joint][0],
            source.fused_joints[joint][1],
            source.fused_joints[joint][2]};
}

void put(PoseResult* pose, int landmark, Vec3 position, float confidence,
         std::uint8_t tracking_state) {
    pose->world[landmark][0] = position.x;
    pose->world[landmark][1] = position.y;
    pose->world[landmark][2] = position.z;
    pose->vis[landmark] = confidence;
    pose->tracking_state[landmark] = tracking_state;
}

void copy_joint(PoseResult* pose, int landmark,
                const anygear::spike::PoseFrame& source, int joint) {
    put(pose, landmark, joint_position(source, joint),
        source.fused_confidence[joint],
        source.fused_tracking_state[joint]);
}

void synthesize_hand(PoseResult* pose,
                     const anygear::spike::PoseFrame& source,
                     bool left, Vec3 body_right, Vec3 body_forward) {
    const int shoulder_joint = left
        ? spike_left_shoulder : spike_right_shoulder;
    const int elbow_joint = left ? spike_left_elbow : spike_right_elbow;
    const int hand_joint = left ? spike_left_hand : spike_right_hand;
    const Vec3 shoulder = joint_position(source, shoulder_joint);
    const Vec3 elbow = joint_position(source, elbow_joint);
    const Vec3 hand = joint_position(source, hand_joint);
    const Vec3 outward = left ? mul(body_right, -1.0f) : body_right;
    const Vec3 upper_arm = normalized_or(sub(elbow, shoulder), outward);
    const Vec3 direction = normalized_or(sub(hand, elbow), upper_arm);
    const Vec3 wrist = add(hand, mul(direction, -0.075f));
    const Vec3 tip = add(hand, mul(direction, 0.070f));

    // SPiKE/ITOP has no wrist or thumb joint. A body-right offset becomes
    // parallel to the forearm in a T-pose, leaving VisionPose's wrist->hand
    // and wrist->thumb axes nearly singular. Use a neutral palm-facing-body
    // anatomical frame instead: the thumb axis is perpendicular to the
    // forearm, points outward on both sides, and remains well-conditioned as
    // the arm moves between hanging and horizontal poses. This is a stable
    // fallback, not a claim that the 15-joint model observes wrist roll.
    Vec3 thumb_axis = left ? cross(body_forward, direction)
                           : cross(direction, body_forward);
    thumb_axis = normalized_or(thumb_axis, outward);
    if (dot(thumb_axis, outward) < 0.0f) {
        thumb_axis = mul(thumb_axis, -1.0f);
    }
    const Vec3 thumb = add(add(wrist, mul(direction, 0.035f)),
                           mul(thumb_axis, 0.055f));
    const float confidence = source.fused_confidence[hand_joint];
    const std::uint8_t hand_state = source.fused_tracking_state[hand_joint];
    const std::uint8_t wrist_state = std::min(
        hand_state, source.fused_tracking_state[elbow_joint]);

    put(pose, left ? MP_LeftWrist : MP_RightWrist,
        wrist, std::min(confidence, source.fused_confidence[elbow_joint]),
        wrist_state);
    put(pose, left ? MP_LeftPinky : MP_RightPinky,
        hand, confidence, hand_state);
    put(pose, left ? MP_LeftIndex : MP_RightIndex,
        tip, confidence, hand_state);
    put(pose, left ? MP_LeftThumb : MP_RightThumb,
        thumb, confidence, hand_state);
}

void synthesize_foot(PoseResult* pose,
                     const anygear::spike::PoseFrame& source,
                     bool left, Vec3 body_up, Vec3 body_forward) {
    const int foot_joint = left ? spike_left_foot : spike_right_foot;
    const int knee_joint = left ? spike_left_knee : spike_right_knee;
    const Vec3 foot = joint_position(source, foot_joint);
    const Vec3 knee = joint_position(source, knee_joint);
    const Vec3 leg_up = normalized_or(sub(knee, foot), body_up);
    const Vec3 foot_forward = normalized_or(
        sub(body_forward, mul(leg_up, dot(body_forward, leg_up))),
        body_forward);
    const Vec3 heel = add(add(foot, mul(foot_forward, -0.055f)),
                          mul(body_up, -0.015f));
    const Vec3 toe = add(add(foot, mul(foot_forward, 0.180f)),
                         mul(body_up, -0.025f));
    const float confidence = source.fused_confidence[foot_joint];
    const std::uint8_t state = source.fused_tracking_state[foot_joint];

    put(pose, left ? MP_LeftAnkle : MP_RightAnkle,
        foot, confidence, state);
    put(pose, left ? MP_LeftHeel : MP_RightHeel,
        heel, confidence, state);
    put(pose, left ? MP_LeftFootIndex : MP_RightFootIndex,
        toe, confidence, state);
}

} // namespace

void map_spike_pose(const anygear::spike::PoseFrame& source,
                    PoseResult* output) {
    if (!output) return;
    *output = PoseResult{};
    output->valid =
        (source.flags & anygear::spike::pose_fused_valid) != 0;
    if (!output->valid) return;
    output->has_tracking_state = true;

    constexpr Vec3 world_up{0.0f, 1.0f, 0.0f};
    constexpr Vec3 camera_facing{0.0f, 0.0f, -1.0f};
    const Vec3 head = joint_position(source, spike_head);
    const Vec3 neck = joint_position(source, spike_neck);
    const Vec3 left_shoulder =
        joint_position(source, spike_left_shoulder);
    const Vec3 right_shoulder =
        joint_position(source, spike_right_shoulder);
    const Vec3 left_hip = joint_position(source, spike_left_hip);
    const Vec3 right_hip = joint_position(source, spike_right_hip);
    const Vec3 shoulder_center = mul(add(left_shoulder, right_shoulder), 0.5f);
    const Vec3 hip_center = mul(add(left_hip, right_hip), 0.5f);
    const Vec3 head_up = normalized_or(sub(head, neck), world_up);
    Vec3 body_up = normalized_or(sub(shoulder_center, hip_center), head_up);
    const Vec3 shoulder_right = normalized_or(
        sub(right_shoulder, left_shoulder), {1.0f, 0.0f, 0.0f});
    const Vec3 hip_right = normalized_or(
        sub(right_hip, left_hip), shoulder_right);
    Vec3 body_right = normalized_or(
        add(mul(shoulder_right, 0.75f), mul(hip_right, 0.25f)),
        shoulder_right);
    body_right = normalized_or(
        sub(body_right, mul(body_up, dot(body_right, body_up))),
        {1.0f, 0.0f, 0.0f});
    Vec3 body_forward = normalized_or(
        cross(body_up, body_right), camera_facing);
    if (dot(body_forward, camera_facing) < 0.0f) {
        body_forward = mul(body_forward, -1.0f);
    }
    body_right = normalized_or(cross(body_forward, body_up), body_right);

    const float face_confidence = source.fused_confidence[spike_head];
    const std::uint8_t face_state =
        source.fused_tracking_state[spike_head];
    const Vec3 eye_plane = add(add(head, mul(body_forward, 0.055f)),
                               mul(body_up, 0.010f));
    const Vec3 left_eye = add(eye_plane, mul(body_right, -0.032f));
    const Vec3 right_eye = add(eye_plane, mul(body_right, 0.032f));
    const Vec3 nose = add(add(head, mul(body_forward, 0.095f)),
                          mul(body_up, -0.012f));
    const Vec3 mouth_plane = add(add(head, mul(body_forward, 0.065f)),
                                 mul(body_up, -0.055f));

    put(output, MP_Nose, nose, face_confidence, face_state);
    put(output, MP_LeftEyeInner,
        add(left_eye, mul(body_right, 0.012f)),
        face_confidence, face_state);
    put(output, MP_LeftEye, left_eye, face_confidence, face_state);
    put(output, MP_LeftEyeOuter,
        add(left_eye, mul(body_right, -0.012f)),
        face_confidence, face_state);
    put(output, MP_RightEyeInner,
        add(right_eye, mul(body_right, -0.012f)),
        face_confidence, face_state);
    put(output, MP_RightEye, right_eye, face_confidence, face_state);
    put(output, MP_RightEyeOuter,
        add(right_eye, mul(body_right, 0.012f)),
        face_confidence, face_state);
    put(output, MP_LeftEar, add(head, mul(body_right, -0.075f)),
        face_confidence, face_state);
    put(output, MP_RightEar, add(head, mul(body_right, 0.075f)),
        face_confidence, face_state);
    put(output, MP_MouthLeft,
        add(mouth_plane, mul(body_right, -0.026f)),
        face_confidence, face_state);
    put(output, MP_MouthRight,
        add(mouth_plane, mul(body_right, 0.026f)),
        face_confidence, face_state);

    copy_joint(output, MP_LeftShoulder, source, spike_left_shoulder);
    copy_joint(output, MP_RightShoulder, source, spike_right_shoulder);
    copy_joint(output, MP_LeftElbow, source, spike_left_elbow);
    copy_joint(output, MP_RightElbow, source, spike_right_elbow);
    synthesize_hand(output, source, true, body_right, body_forward);
    synthesize_hand(output, source, false, body_right, body_forward);
    copy_joint(output, MP_LeftHip, source, spike_left_hip);
    copy_joint(output, MP_RightHip, source, spike_right_hip);
    copy_joint(output, MP_LeftKnee, source, spike_left_knee);
    copy_joint(output, MP_RightKnee, source, spike_right_knee);
    synthesize_foot(output, source, true, body_up, body_forward);
    synthesize_foot(output, source, false, body_up, body_forward);

    float confidence = 0.0f;
    for (const float value : source.fused_confidence) confidence += value;
    output->confidence = confidence / anygear::spike::kJointCount;
}

} // namespace vp4u
