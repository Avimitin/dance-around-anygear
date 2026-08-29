// Minimal MediaPipe Tasks C ABI used by dance-around-anygear.
//
// The declarations are derived from google-ai-edge/mediapipe v1.0.0
// (commit 6d31f1ebc3284db74d211d62bdc4f0a0c29ea120), Apache-2.0.
// Keeping this as a compact ABI surface lets the plugin load the official
// libmediapipe.dll at runtime without a Python installation or import library.
#ifndef ANYGEAR_MEDIAPIPE_C_API_H
#define ANYGEAR_MEDIAPIPE_C_API_H

#include <cstdint>

namespace vp4u::mp {

enum Status : int {
    kOk = 0,
};

enum Delegate : int {
    kDelegateCpu = 0,
    kDelegateGpu = 1,
    kDelegateEdgeTpuNnapi = 2,
};

enum HostEnvironment : int {
    kHostEnvironmentUnknown = 0,
};

enum HostSystem : int {
    kHostSystemUnknown = 0,
    kHostSystemLinux = 1,
    kHostSystemMac = 2,
    kHostSystemWindows = 3,
};

struct BaseOptions {
    const char* model_asset_buffer;
    unsigned int model_asset_buffer_count;
    const char* model_asset_path;
    int file_descriptor;
    Delegate delegate;
    HostEnvironment host_environment;
    HostSystem host_system;
    const char* host_version;
    const char* ca_bundle_path;
    const char* app_id;
    const char* app_version;
};

enum RunningMode : int {
    kRunningModeImage = 1,
    kRunningModeVideo = 2,
    kRunningModeLiveStream = 3,
};

enum ImageFormat : int {
    kImageFormatUnknown = 0,
    kImageFormatSrgb = 1,
    kImageFormatSrgba = 2,
};

struct ImageInternal;
using ImagePtr = ImageInternal*;

struct PoseLandmarkerInternal;
using PoseLandmarkerPtr = PoseLandmarkerInternal*;

struct Landmark {
    float x;
    float y;
    float z;
    bool has_visibility;
    float visibility;
    bool has_presence;
    float presence;
    char* name;
};

struct NormalizedLandmark {
    float x;
    float y;
    float z;
    bool has_visibility;
    float visibility;
    bool has_presence;
    float presence;
    char* name;
};

struct Landmarks {
    Landmark* landmarks;
    std::uint32_t landmarks_count;
};

struct NormalizedLandmarks {
    NormalizedLandmark* landmarks;
    std::uint32_t landmarks_count;
};

struct PoseLandmarkerResult {
    ImagePtr* segmentation_masks;
    std::uint32_t segmentation_masks_count;
    NormalizedLandmarks* pose_landmarks;
    std::uint32_t pose_landmarks_count;
    Landmarks* pose_world_landmarks;
    std::uint32_t pose_world_landmarks_count;
};

using ResultCallback = void (*)(Status, const PoseLandmarkerResult*,
                                ImagePtr, std::int64_t);

struct PoseLandmarkerOptions {
    BaseOptions base_options;
    RunningMode running_mode;
    int num_poses;
    float min_pose_detection_confidence;
    float min_pose_presence_confidence;
    float min_tracking_confidence;
    bool output_segmentation_masks;
    ResultCallback result_callback;
};

static_assert(sizeof(BaseOptions) == 72, "MediaPipe v1 BaseOptions ABI drift");
static_assert(sizeof(Landmark) == 40, "MediaPipe v1 Landmark ABI drift");
static_assert(sizeof(PoseLandmarkerResult) == 48,
              "MediaPipe v1 result ABI drift");
static_assert(sizeof(PoseLandmarkerOptions) == 104,
              "MediaPipe v1 options ABI drift");

using ErrorFreeFn = void (*)(char*);
using ImageCreateFromUint8DataFn = Status (*)(
    ImageFormat, int, int, const std::uint8_t*, int, ImagePtr*, char**);
using ImageFreeFn = void (*)(ImagePtr);
using PoseLandmarkerCreateFn = Status (*)(
    PoseLandmarkerOptions*, PoseLandmarkerPtr*, char**);
using PoseLandmarkerDetectForVideoFn = Status (*)(
    PoseLandmarkerPtr, ImagePtr, const void*, std::int64_t,
    PoseLandmarkerResult*, char**);
using PoseLandmarkerCloseResultFn = void (*)(PoseLandmarkerResult*);
using PoseLandmarkerCloseFn = Status (*)(PoseLandmarkerPtr, char**);

} // namespace vp4u::mp

#endif // ANYGEAR_MEDIAPIPE_C_API_H
