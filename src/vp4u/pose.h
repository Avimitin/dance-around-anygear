// pose.h - MediaPipe Pose Landmarker behind a small VP4U-facing interface.
// Dynamically loads the official C runtime (no import-lib/Python dependency).
#ifndef VP4U_POSE_H
#define VP4U_POSE_H

#include <cstdint>
#include <string>

namespace vp4u {

// MediaPipe Pose Landmarker indices (the model's 33-landmark topology).
enum MpLandmark {
    MP_Nose = 0,
    MP_LeftEyeInner, MP_LeftEye, MP_LeftEyeOuter,
    MP_RightEyeInner, MP_RightEye, MP_RightEyeOuter,
    MP_LeftEar, MP_RightEar, MP_MouthLeft, MP_MouthRight,
    MP_LeftShoulder, MP_RightShoulder, MP_LeftElbow, MP_RightElbow,
    MP_LeftWrist, MP_RightWrist, MP_LeftPinky, MP_RightPinky,
    MP_LeftIndex, MP_RightIndex, MP_LeftThumb, MP_RightThumb,
    MP_LeftHip, MP_RightHip, MP_LeftKnee, MP_RightKnee,
    MP_LeftAnkle, MP_RightAnkle, MP_LeftHeel, MP_RightHeel,
    MP_LeftFootIndex, MP_RightFootIndex
};
constexpr int kMpLandmarkCount = 33;

struct PoseResult {
    bool  valid = false;        // a person was detected this frame
    // Hardware/model backends may supply an exact per-landmark 0/1/2 state.
    // MediaPipe and existing hardware paths retain visibility-based mapping.
    bool  has_tracking_state = false;
    float confidence = 0.f;     // mean visibility of key joints
    // Stage-space meters for each MediaPipe landmark (x right, y up, z depth).
    float world[kMpLandmarkCount][3] {};
    // Source-frame pixel coordinates (debug / future 2D output).
    float pixel[kMpLandmarkCount][2] {};
    float vis[kMpLandmarkCount] {};
    std::uint8_t tracking_state[kMpLandmarkCount] {};
};

inline int pose_tracking_state(const PoseResult& pose, int landmark) {
    if (pose.has_tracking_state) {
        const int state = pose.tracking_state[landmark];
        return state > 2 ? 2 : state;
    }
    return pose.vis[landmark] > 0.5f ? 2 : 1;
}

inline void smooth_pose_landmark(PoseResult* pose, int landmark, float alpha,
                                 float previous[3], bool* have_previous) {
    float* current = pose->world[landmark];
    if (pose->has_tracking_state && pose->tracking_state[landmark] == 0) {
        if (*have_previous) {
            for (int component = 0; component < 3; ++component) {
                current[component] = previous[component];
            }
        }
        return;
    }
    if (*have_previous) {
        for (int component = 0; component < 3; ++component) {
            current[component] = alpha * current[component] +
                (1.0f - alpha) * previous[component];
        }
    }
    for (int component = 0; component < 3; ++component) {
        previous[component] = current[component];
    }
    *have_previous = true;
}

// Simple stage calibration: where the detected hip center lands on the stage,
// and the assumed torso (shoulder-mid to hip-mid) length in meters.
struct PoseCalib {
    float hip_stage_x = 0.9f;    // StageBox center X
    float hip_stage_y = 1.0f;    // assumed standing hip height
    float hip_stage_z = 0.75f;   // StageBox center Z
    float torso_len_m = 0.52f;   // absolute scale anchor
    float z_damping   = 0.7f;    // suppress monocular depth noise
    float ema_alpha   = 0.55f;   // landmark smoothing (0=heavy, 1=none)
};

class PoseEngine {
public:
    PoseEngine();
    ~PoseEngine();

    // runtime_dir: directory containing libmediapipe.dll.
    // model_path: path to a compatible Pose Landmarker .task bundle.
    bool init(const std::wstring& runtime_dir, const std::string& model_path,
              const PoseCalib& calib, std::string* err);
    bool ok() const;
    void shutdown();

    // bgr: w*h*3 BGR8 top-down frame. Fills `out`.
    bool infer(const uint8_t* bgr, int w, int h, PoseResult* out);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace vp4u

#endif // VP4U_POSE_H
