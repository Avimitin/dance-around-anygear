#include "steamvr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace vp4u {
namespace {

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
    return magnitude > 1.0e-5f ? mul(value, 1.f / magnitude) : fallback;
}

Vec3 lerp(Vec3 a, Vec3 b, float t) {
    return add(a, mul(sub(b, a), t));
}

Vec3 position_of(const SteamVrDevicePose& pose) {
    return {pose.position[0], pose.position[1], pose.position[2]};
}

Vec3 local_axis(const SteamVrDevicePose& pose, int column) {
    return {pose.rotation[0][column],
            pose.rotation[1][column],
            pose.rotation[2][column]};
}

Vec3 horizontal_or(Vec3 value, Vec3 fallback) {
    value.y = 0.f;
    return normalized_or(value, fallback);
}

size_t index_of(SteamVrBodyRole role) {
    return static_cast<size_t>(role);
}

const SteamVrDevicePose& pose_of(const SteamVrPoseFrame& frame,
                                 SteamVrBodyRole role) {
    return frame.device[index_of(role)];
}

struct Point {
    Vec3 position;
    float visibility;
};

} // namespace

const char* steamvr_body_role_name(SteamVrBodyRole role) {
    switch (role) {
        case SteamVrBodyRole::Head: return "head";
        case SteamVrBodyRole::LeftHand: return "left-hand";
        case SteamVrBodyRole::RightHand: return "right-hand";
        case SteamVrBodyRole::Waist: return "waist";
        case SteamVrBodyRole::Chest: return "chest";
        case SteamVrBodyRole::LeftShoulder: return "left-shoulder";
        case SteamVrBodyRole::RightShoulder: return "right-shoulder";
        case SteamVrBodyRole::LeftElbow: return "left-elbow";
        case SteamVrBodyRole::RightElbow: return "right-elbow";
        case SteamVrBodyRole::LeftKnee: return "left-knee";
        case SteamVrBodyRole::RightKnee: return "right-knee";
        case SteamVrBodyRole::LeftFoot: return "left-foot";
        case SteamVrBodyRole::RightFoot: return "right-foot";
        case SteamVrBodyRole::Count: break;
    }
    return "unknown";
}

bool steamvr_body_role_from_setting(const std::string& setting,
                                    SteamVrBodyRole* role) {
    if (!role) return false;
    std::string value = setting;
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    struct Match { const char* text; SteamVrBodyRole role; };
    static constexpr Match matches[] = {
        {"trackerrole_leftfoot", SteamVrBodyRole::LeftFoot},
        {"trackerrole_rightfoot", SteamVrBodyRole::RightFoot},
        {"trackerrole_leftshoulder", SteamVrBodyRole::LeftShoulder},
        {"trackerrole_rightshoulder", SteamVrBodyRole::RightShoulder},
        {"trackerrole_leftelbow", SteamVrBodyRole::LeftElbow},
        {"trackerrole_rightelbow", SteamVrBodyRole::RightElbow},
        {"trackerrole_leftknee", SteamVrBodyRole::LeftKnee},
        {"trackerrole_rightknee", SteamVrBodyRole::RightKnee},
        {"trackerrole_waist", SteamVrBodyRole::Waist},
        {"trackerrole_chest", SteamVrBodyRole::Chest},
    };
    for (const Match& match : matches) {
        if (value == match.text) {
            *role = match.role;
            return true;
        }
    }
    return false;
}

bool map_steamvr_pose(const SteamVrPoseFrame& frame,
                      const SteamVrTrackingOptions& options,
                      SteamVrCalibration* calibration,
                      PoseResult* output) {
    if (!calibration || !output) return false;
    *output = PoseResult();

    const SteamVrDevicePose& head_pose = pose_of(frame, SteamVrBodyRole::Head);
    const SteamVrDevicePose& left_hand_pose =
        pose_of(frame, SteamVrBodyRole::LeftHand);
    const SteamVrDevicePose& right_hand_pose =
        pose_of(frame, SteamVrBodyRole::RightHand);
    const SteamVrDevicePose& waist_pose = pose_of(frame, SteamVrBodyRole::Waist);
    const SteamVrDevicePose& left_foot_pose =
        pose_of(frame, SteamVrBodyRole::LeftFoot);
    const SteamVrDevicePose& right_foot_pose =
        pose_of(frame, SteamVrBodyRole::RightFoot);

    if (!head_pose.tracked || !left_hand_pose.tracked ||
        !right_hand_pose.tracked) {
        return true;
    }
    if (options.require_waist && !waist_pose.tracked) return true;
    if (options.require_feet &&
        (!left_foot_pose.tracked || !right_foot_pose.tracked)) {
        return true;
    }

    constexpr Vec3 world_up{0.f, 1.f, 0.f};
    const Vec3 head_eye = position_of(head_pose);
    Vec3 head_right = horizontal_or(local_axis(head_pose, 0), {1.f, 0.f, 0.f});
    Vec3 head_forward = horizontal_or(
        mul(local_axis(head_pose, 2), -1.f), {0.f, 0.f, -1.f});

    Vec3 waist = waist_pose.tracked
        ? position_of(waist_pose)
        : add(head_eye, {0.f, -0.72f, 0.04f});

    Vec3 left_foot = left_foot_pose.tracked
        ? position_of(left_foot_pose)
        : add(waist, {-0.14f, -waist.y + 0.04f, 0.02f});
    Vec3 right_foot = right_foot_pose.tracked
        ? position_of(right_foot_pose)
        : add(waist, {0.14f, -waist.y + 0.04f, 0.02f});

    Vec3 feet_right = horizontal_or(sub(right_foot, left_foot), head_right);
    if (dot(feet_right, head_right) < 0.f) feet_right = mul(feet_right, -1.f);
    Vec3 body_right = feet_right;
    Vec3 body_forward = normalized_or(cross(world_up, body_right), head_forward);
    if (dot(body_forward, head_forward) < 0.f) body_forward = mul(body_forward, -1.f);

    if (!calibration->ready) {
        calibration->origin[0] = options.auto_center ? waist.x : 0.f;
        calibration->origin[1] = options.auto_center ? waist.y : 0.f;
        calibration->origin[2] = options.auto_center ? waist.z : 0.f;
        calibration->right[0] = body_right.x;
        calibration->right[1] = body_right.y;
        calibration->right[2] = body_right.z;
        calibration->forward[0] = body_forward.x;
        calibration->forward[1] = body_forward.y;
        calibration->forward[2] = body_forward.z;
        calibration->ready = true;
    }

    const Vec3 origin{calibration->origin[0], calibration->origin[1],
                      calibration->origin[2]};
    const Vec3 stage_right{calibration->right[0], calibration->right[1],
                           calibration->right[2]};
    const Vec3 stage_forward{calibration->forward[0],
                             calibration->forward[1],
                             calibration->forward[2]};
    const float yaw_sign = options.face_to_face ? -1.f : 1.f;
    const auto to_stage = [&](Vec3 point) {
        const Vec3 relative = sub(point, origin);
        return Vec3{
            options.stage_hip_x + yaw_sign * dot(relative, stage_right),
            options.stage_hip_y + relative.y,
            options.stage_hip_z + yaw_sign * dot(relative, stage_forward)};
    };

    // The HMD origin is approximately between the eyes. Build a compact rigid
    // face around it so consumers never receive coincident head landmarks.
    const Vec3 head_up = normalized_or(local_axis(head_pose, 1), world_up);
    const Vec3 face_forward = normalized_or(
        mul(local_axis(head_pose, 2), -1.f), body_forward);
    const Vec3 face_right = normalized_or(local_axis(head_pose, 0), body_right);
    const Vec3 head_center = add(head_eye, mul(face_forward, -0.055f));
    const Vec3 eye_plane = add(head_eye, mul(face_forward, 0.025f));
    const Vec3 nose = add(add(head_eye, mul(face_forward, 0.080f)),
                          mul(head_up, -0.018f));
    const Vec3 left_eye = add(eye_plane, mul(face_right, -0.032f));
    const Vec3 right_eye = add(eye_plane, mul(face_right, 0.032f));
    const Vec3 mouth_plane = add(add(head_eye, mul(face_forward, 0.055f)),
                                 mul(head_up, -0.070f));

    const SteamVrDevicePose& chest_pose = pose_of(frame, SteamVrBodyRole::Chest);
    const Vec3 shoulder_mid = chest_pose.tracked
        ? position_of(chest_pose)
        : lerp(waist, head_center, 0.68f);

    const SteamVrDevicePose& left_shoulder_pose =
        pose_of(frame, SteamVrBodyRole::LeftShoulder);
    const SteamVrDevicePose& right_shoulder_pose =
        pose_of(frame, SteamVrBodyRole::RightShoulder);
    const Vec3 left_shoulder = left_shoulder_pose.tracked
        ? position_of(left_shoulder_pose)
        : add(shoulder_mid, mul(body_right, -0.195f));
    const Vec3 right_shoulder = right_shoulder_pose.tracked
        ? position_of(right_shoulder_pose)
        : add(shoulder_mid, mul(body_right, 0.195f));

    const Vec3 left_wrist = position_of(left_hand_pose);
    const Vec3 right_wrist = position_of(right_hand_pose);
    const SteamVrDevicePose& left_elbow_pose =
        pose_of(frame, SteamVrBodyRole::LeftElbow);
    const SteamVrDevicePose& right_elbow_pose =
        pose_of(frame, SteamVrBodyRole::RightElbow);
    const Vec3 left_elbow = left_elbow_pose.tracked
        ? position_of(left_elbow_pose)
        : add(add(lerp(left_shoulder, left_wrist, 0.52f),
                      mul(world_up, -0.055f)),
              mul(body_forward, 0.035f));
    const Vec3 right_elbow = right_elbow_pose.tracked
        ? position_of(right_elbow_pose)
        : add(add(lerp(right_shoulder, right_wrist, 0.52f),
                      mul(world_up, -0.055f)),
              mul(body_forward, 0.035f));

    const Vec3 left_hip = add(add(waist, mul(body_right, -0.145f)),
                              mul(world_up, -0.055f));
    const Vec3 right_hip = add(add(waist, mul(body_right, 0.145f)),
                               mul(world_up, -0.055f));

    const Vec3 left_ankle = add(add(left_foot, mul(world_up, 0.080f)),
                                mul(body_forward, -0.025f));
    const Vec3 right_ankle = add(add(right_foot, mul(world_up, 0.080f)),
                                 mul(body_forward, -0.025f));
    const SteamVrDevicePose& left_knee_pose =
        pose_of(frame, SteamVrBodyRole::LeftKnee);
    const SteamVrDevicePose& right_knee_pose =
        pose_of(frame, SteamVrBodyRole::RightKnee);
    const Vec3 left_knee = left_knee_pose.tracked
        ? position_of(left_knee_pose)
        : add(lerp(left_hip, left_ankle, 0.52f), mul(body_forward, 0.060f));
    const Vec3 right_knee = right_knee_pose.tracked
        ? position_of(right_knee_pose)
        : add(lerp(right_hip, right_ankle, 0.52f), mul(body_forward, 0.060f));

    const auto make_hand_points = [&](Vec3 wrist, Vec3 elbow,
                                      const SteamVrDevicePose& device,
                                      bool left, Point* pinky, Point* index,
                                      Point* thumb) {
        Vec3 direction = normalized_or(sub(wrist, elbow), body_forward);
        Vec3 device_forward = normalized_or(
            mul(local_axis(device, 2), -1.f), direction);
        if (std::fabs(dot(device_forward, direction)) < 0.20f) {
            device_forward = direction;
        }
        const Vec3 hand = add(wrist, mul(device_forward, 0.045f));
        *pinky = {hand, 1.f};
        *index = {add(hand, mul(device_forward, 0.070f)), 0.75f};
        const float side = left ? 0.032f : -0.032f;
        *thumb = {add(add(hand, mul(device_forward, 0.025f)),
                      mul(body_right, side)), 0.75f};
    };

    std::array<Point, kMpLandmarkCount> point {};
    const auto put = [&](int landmark, Vec3 value, float visibility) {
        point[landmark] = {value, visibility};
    };
    put(MP_Nose, nose, 1.f);
    put(MP_LeftEyeInner, add(left_eye, mul(face_right, 0.012f)), 1.f);
    put(MP_LeftEye, left_eye, 1.f);
    put(MP_LeftEyeOuter, add(left_eye, mul(face_right, -0.012f)), 1.f);
    put(MP_RightEyeInner, add(right_eye, mul(face_right, -0.012f)), 1.f);
    put(MP_RightEye, right_eye, 1.f);
    put(MP_RightEyeOuter, add(right_eye, mul(face_right, 0.012f)), 1.f);
    put(MP_LeftEar, add(head_center, mul(face_right, -0.075f)), 1.f);
    put(MP_RightEar, add(head_center, mul(face_right, 0.075f)), 1.f);
    put(MP_MouthLeft, add(mouth_plane, mul(face_right, -0.026f)), 1.f);
    put(MP_MouthRight, add(mouth_plane, mul(face_right, 0.026f)), 1.f);
    put(MP_LeftShoulder, left_shoulder,
        left_shoulder_pose.tracked ? 1.f : 0.5f);
    put(MP_RightShoulder, right_shoulder,
        right_shoulder_pose.tracked ? 1.f : 0.5f);
    put(MP_LeftElbow, left_elbow, left_elbow_pose.tracked ? 1.f : 0.5f);
    put(MP_RightElbow, right_elbow, right_elbow_pose.tracked ? 1.f : 0.5f);
    put(MP_LeftWrist, left_wrist, 1.f);
    put(MP_RightWrist, right_wrist, 1.f);
    make_hand_points(left_wrist, left_elbow, left_hand_pose, true,
                     &point[MP_LeftPinky], &point[MP_LeftIndex],
                     &point[MP_LeftThumb]);
    make_hand_points(right_wrist, right_elbow, right_hand_pose, false,
                     &point[MP_RightPinky], &point[MP_RightIndex],
                     &point[MP_RightThumb]);
    put(MP_LeftHip, left_hip, waist_pose.tracked ? 0.75f : 0.5f);
    put(MP_RightHip, right_hip, waist_pose.tracked ? 0.75f : 0.5f);
    put(MP_LeftKnee, left_knee, left_knee_pose.tracked ? 1.f : 0.5f);
    put(MP_RightKnee, right_knee, right_knee_pose.tracked ? 1.f : 0.5f);
    put(MP_LeftAnkle, left_ankle, left_foot_pose.tracked ? 0.75f : 0.5f);
    put(MP_RightAnkle, right_ankle, right_foot_pose.tracked ? 0.75f : 0.5f);
    put(MP_LeftHeel, add(left_foot, mul(body_forward, -0.070f)),
        left_foot_pose.tracked ? 1.f : 0.5f);
    put(MP_RightHeel, add(right_foot, mul(body_forward, -0.070f)),
        right_foot_pose.tracked ? 1.f : 0.5f);
    put(MP_LeftFootIndex,
        add(add(left_foot, mul(body_forward, 0.160f)),
            mul(world_up, -0.020f)),
        left_foot_pose.tracked ? 1.f : 0.5f);
    put(MP_RightFootIndex,
        add(add(right_foot, mul(body_forward, 0.160f)),
            mul(world_up, -0.020f)),
        right_foot_pose.tracked ? 1.f : 0.5f);

    for (int i = 0; i < kMpLandmarkCount; ++i) {
        const Vec3 stage = to_stage(point[i].position);
        output->world[i][0] = stage.x;
        output->world[i][1] = stage.y;
        output->world[i][2] = stage.z;
        output->vis[i] = point[i].visibility;
    }

    const int direct_count = 3 + (waist_pose.tracked ? 1 : 0) +
                             (left_foot_pose.tracked ? 1 : 0) +
                             (right_foot_pose.tracked ? 1 : 0);
    // Keep confidence tied to the six-point body even when the user
    // explicitly enables inferred torso/legs for an integration check.
    output->confidence = static_cast<float>(direct_count) / 6.f;
    output->valid = true;
    return true;
}

} // namespace vp4u
