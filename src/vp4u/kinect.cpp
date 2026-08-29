// kinect.cpp - Kinect for Windows v1 color and skeleton source.
// The official SDK supplies the NUI declarations. Kinect10.dll is resolved at
// runtime so the release DLL does not redistribute or link the SDK runtime.
#include "kinect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <windows.h>

#include <NuiApi.h>

#include "webcam.h"   // make_synthetic_bgr8, scale_bgr8_bilinear

namespace vp4u {

static void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[vp4u-kinect] %s\n", buf);
}

namespace nui {
using ::INuiFrameTexture;
using ::NUI_IMAGE_FRAME;
using ::NUI_IMAGE_RESOLUTION;
using ::NUI_IMAGE_RESOLUTION_640x480;
using ::NUI_IMAGE_TYPE;
using ::NUI_IMAGE_TYPE_COLOR;
using ::NUI_LOCKED_RECT;
using ::NUI_SKELETON_DATA;
using ::NUI_SKELETON_FRAME;
using ::NUI_SKELETON_POSITION_COUNT;
using ::NUI_SKELETON_POSITION_HEAD;
using ::NUI_SKELETON_POSITION_HIP_CENTER;
using ::NUI_SKELETON_POSITION_HIP_LEFT;
using ::NUI_SKELETON_POSITION_HIP_RIGHT;
using ::NUI_SKELETON_POSITION_INFERRED;
using ::NUI_SKELETON_POSITION_SHOULDER_CENTER;
using ::NUI_SKELETON_POSITION_SHOULDER_LEFT;
using ::NUI_SKELETON_POSITION_SHOULDER_RIGHT;
using ::NUI_SKELETON_POSITION_SPINE;
using ::NUI_SKELETON_POSITION_TRACKED;
using ::NUI_SKELETON_POSITION_TRACKING_STATE;
using ::NUI_SKELETON_TRACKED;
using ::NUI_TRANSFORM_SMOOTH_PARAMETERS;
using ::Vector4;

inline constexpr int kSkeletonCount = NUI_SKELETON_COUNT;
inline constexpr DWORD kInitializeDepthAndPlayer =
    NUI_INITIALIZE_FLAG_USES_DEPTH_AND_PLAYER_INDEX;
inline constexpr DWORD kInitializeColor = NUI_INITIALIZE_FLAG_USES_COLOR;
inline constexpr DWORD kInitializeSkeleton = NUI_INITIALIZE_FLAG_USES_SKELETON;
inline constexpr DWORD kQualityClippedRight = NUI_SKELETON_QUALITY_CLIPPED_RIGHT;
inline constexpr DWORD kQualityClippedLeft = NUI_SKELETON_QUALITY_CLIPPED_LEFT;
inline constexpr DWORD kQualityClippedTop = NUI_SKELETON_QUALITY_CLIPPED_TOP;
inline constexpr DWORD kQualityClippedBottom = NUI_SKELETON_QUALITY_CLIPPED_BOTTOM;
} // namespace nui

// --------------------------------------------- dynamically loaded API -----
struct NuiApi {
    HMODULE dll = nullptr;
    HRESULT (WINAPI* NuiInitialize)(DWORD dwFlags) = nullptr;
    VOID    (WINAPI* NuiShutdown)() = nullptr;
    HRESULT (WINAPI* NuiImageStreamOpen)(nui::NUI_IMAGE_TYPE, nui::NUI_IMAGE_RESOLUTION,
                                         DWORD, DWORD, HANDLE, HANDLE*) = nullptr;
    HRESULT (WINAPI* NuiImageStreamGetNextFrame)(HANDLE, DWORD,
                                                 const nui::NUI_IMAGE_FRAME**) = nullptr;
    HRESULT (WINAPI* NuiImageStreamReleaseFrame)(HANDLE, const nui::NUI_IMAGE_FRAME*) = nullptr;
    HRESULT (WINAPI* NuiSkeletonTrackingEnable)(HANDLE, DWORD) = nullptr;
    HRESULT (WINAPI* NuiSkeletonGetNextFrame)(DWORD, nui::NUI_SKELETON_FRAME*) = nullptr;
    HRESULT (WINAPI* NuiTransformSmooth)(nui::NUI_SKELETON_FRAME*,
                                         const nui::NUI_TRANSFORM_SMOOTH_PARAMETERS*) = nullptr;
    HRESULT (WINAPI* NuiCameraElevationGetAngle)(LONG*) = nullptr;

    template <typename T> T proc(const char* name, bool* ok) {
        FARPROC p = GetProcAddress(dll, name);
        if (!p) { logf("GetProcAddress(%s) failed", name); *ok = false; }
        T fn = nullptr;
        static_assert(sizeof fn == sizeof p, "pointer size");
        std::memcpy(&fn, &p, sizeof fn); // avoids -Wcast-function-type
        return fn;
    }

    bool load() {
        dll = LoadLibraryW(L"Kinect10.dll");
        if (!dll) { logf("LoadLibrary Kinect10.dll failed (runtime not installed?)"); return false; }
        bool ok = true;
        NuiInitialize              = proc<decltype(NuiInitialize)>("NuiInitialize", &ok);
        NuiShutdown                = proc<decltype(NuiShutdown)>("NuiShutdown", &ok);
        NuiImageStreamOpen         = proc<decltype(NuiImageStreamOpen)>("NuiImageStreamOpen", &ok);
        NuiImageStreamGetNextFrame = proc<decltype(NuiImageStreamGetNextFrame)>("NuiImageStreamGetNextFrame", &ok);
        NuiImageStreamReleaseFrame = proc<decltype(NuiImageStreamReleaseFrame)>("NuiImageStreamReleaseFrame", &ok);
        NuiSkeletonTrackingEnable  = proc<decltype(NuiSkeletonTrackingEnable)>("NuiSkeletonTrackingEnable", &ok);
        NuiSkeletonGetNextFrame    = proc<decltype(NuiSkeletonGetNextFrame)>("NuiSkeletonGetNextFrame", &ok);
        NuiTransformSmooth         = proc<decltype(NuiTransformSmooth)>("NuiTransformSmooth", &ok);
        NuiCameraElevationGetAngle = proc<decltype(NuiCameraElevationGetAngle)>("NuiCameraElevationGetAngle", &ok);
        return ok;
    }
};

// ------------------------------------------- NUI joint -> MP landmark -----
static float nui_vis(const nui::NUI_SKELETON_DATA& sk, int j) {
    switch (sk.eSkeletonPositionTrackingState[j]) {
        case nui::NUI_SKELETON_POSITION_TRACKED:  return 1.0f;
        case nui::NUI_SKELETON_POSITION_INFERRED: return 0.5f;
        default:                                  return 0.f;
    }
}

struct Vec3f { float x, y, z; };

static Vec3f vec_of(const nui::Vector4& v) { return {v.x, v.y, v.z}; }
static Vec3f vec_add(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3f vec_sub(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3f vec_mul(Vec3f v, float s) { return {v.x * s, v.y * s, v.z * s}; }
static Vec3f vec_cross(Vec3f a, Vec3f b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
static float vec_len(Vec3f v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
static Vec3f vec_normalize_or(Vec3f v, Vec3f fallback) {
    const float length = vec_len(v);
    return length > 1.0e-5f ? vec_mul(v, 1.0f / length) : fallback;
}

static bool vec_is_valid(Vec3f v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < 10.f && std::fabs(v.y) < 10.f &&
           v.z > 0.10f && v.z < 10.f;
}

static Vec3f vec_limit_delta(Vec3f origin, Vec3f target, float max_delta,
                             bool* limited) {
    const Vec3f delta = vec_sub(target, origin);
    const float length = vec_len(delta);
    if (length <= max_delta || length <= 1.0e-5f) return target;
    if (limited) *limited = true;
    return vec_add(origin, vec_mul(delta, max_delta / length));
}

static void vec_store(nui::Vector4* dst, Vec3f v) {
    dst->x = v.x;
    dst->y = v.y;
    dst->z = v.z;
    // Kinect uses w=1 for valid skeleton points. Preserve that convention for
    // repaired points even though the VP4U conversion only consumes xyz.
    dst->w = 1.f;
}

// Kinect's 20 joints are already ordered parent-before-child. Keeping the
// previous child-to-parent vector is a stable fallback when NUI marks a distal
// joint INFERRED or NOT_TRACKED: the limb follows body translation without
// accepting the endpoint guess that causes a hand/foot to fly away.
static const int kJointParent[nui::NUI_SKELETON_POSITION_COUNT] = {
    -1, // HipCenter
     0, // Spine
     1, // ShoulderCenter
     2, // Head
     2, 4, 5, 6,   // left arm
     2, 8, 9, 10,  // right arm
     0, 12, 13, 14, // left leg
     0, 16, 17, 18  // right leg
};

static int count_quality_bits(DWORD flags) {
    int count = 0;
    for (; flags; flags &= flags - 1) ++count;
    return count;
}

static void format_clip_flags(DWORD flags, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    const auto append = [&](const char* text) {
        if (out[0] != '\0') std::strncat(out, "|", out_size - std::strlen(out) - 1);
        std::strncat(out, text, out_size - std::strlen(out) - 1);
    };
    if (flags & nui::kQualityClippedRight) append("right");
    if (flags & nui::kQualityClippedLeft) append("left");
    if (flags & nui::kQualityClippedTop) append("top");
    if (flags & nui::kQualityClippedBottom) append("bottom");
    if (out[0] == '\0') std::strncpy(out, "none", out_size - 1);
    out[out_size - 1] = '\0';
}

// NUI positions are metric camera coordinates (+Y up, +Z depth, +X to the
// camera's left in this right-handed system). Copy the raw values here; the
// VP4U backend boundary converts X and applies configurable sensor extrinsics.
// Pixel coords are left at 0 (debug-only field).
static void fill_pose(const nui::NUI_SKELETON_DATA& sk, PoseResult* out) {
    *out = PoseResult(); // zero all landmarks, valid=false until set below
    auto set = [&](int mp, int j, float vscale) {
        const nui::Vector4& v = sk.SkeletonPositions[j];
        out->world[mp][0] = v.x;
        out->world[mp][1] = v.y;
        out->world[mp][2] = v.z;
        out->pixel[mp][0] = out->pixel[mp][1] = 0.f;
        out->vis[mp] = nui_vis(sk, j) * vscale;
    };
    auto set_vec = [&](int mp, Vec3f v, float visibility) {
        out->world[mp][0] = v.x;
        out->world[mp][1] = v.y;
        out->world[mp][2] = v.z;
        out->pixel[mp][0] = out->pixel[mp][1] = 0.f;
        out->vis[mp] = visibility;
    };
    using namespace nui;

    // Kinect v1 has only one HEAD point. Copying it into Nose/Eyes/Ears makes
    // VisionPosePoseEstimator and its 3D skeleton links call LookRotation with
    // a zero vector every frame. Infer a compact, rigid face basis from the
    // tracked shoulder line and head-up direction instead. This supplies a
    // stable facing vector without pretending Kinect v1 has facial tracking.
    const Vec3f head = vec_of(sk.SkeletonPositions[NUI_SKELETON_POSITION_HEAD]);
    const Vec3f shoulderCenter = vec_of(
        sk.SkeletonPositions[NUI_SKELETON_POSITION_SHOULDER_CENTER]);
    const Vec3f shoulderLeft = vec_of(
        sk.SkeletonPositions[NUI_SKELETON_POSITION_SHOULDER_LEFT]);
    const Vec3f shoulderRight = vec_of(
        sk.SkeletonPositions[NUI_SKELETON_POSITION_SHOULDER_RIGHT]);
    const Vec3f bodyRight = vec_normalize_or(
        vec_sub(shoulderRight, shoulderLeft), {1.f, 0.f, 0.f});
    const Vec3f bodyUp = vec_normalize_or(
        vec_sub(head, shoulderCenter), {0.f, 1.f, 0.f});
    // In raw NUI camera space the tracked player faces the sensor (-Z).
    const Vec3f bodyForward = vec_normalize_or(
        vec_cross(bodyUp, bodyRight), {0.f, 0.f, -1.f});
    const float faceVisibility = std::min(
        nui_vis(sk, NUI_SKELETON_POSITION_HEAD),
        std::min(nui_vis(sk, NUI_SKELETON_POSITION_SHOULDER_LEFT),
                 nui_vis(sk, NUI_SKELETON_POSITION_SHOULDER_RIGHT)));

    const Vec3f leftEar = vec_add(head, vec_mul(bodyRight, -0.075f));
    const Vec3f rightEar = vec_add(head, vec_mul(bodyRight, 0.075f));
    const Vec3f nose = vec_add(vec_add(head, vec_mul(bodyForward, 0.105f)),
                              vec_mul(bodyUp, -0.005f));
    const Vec3f eyePlane = vec_add(vec_add(head, vec_mul(bodyForward, 0.080f)),
                                  vec_mul(bodyUp, 0.015f));
    const Vec3f leftEye = vec_add(eyePlane, vec_mul(bodyRight, -0.032f));
    const Vec3f rightEye = vec_add(eyePlane, vec_mul(bodyRight, 0.032f));
    const Vec3f mouthPlane = vec_add(vec_add(head, vec_mul(bodyForward, 0.085f)),
                                    vec_mul(bodyUp, -0.040f));

    set_vec(MP_Nose, nose, faceVisibility);
    set_vec(MP_LeftEyeInner, vec_add(leftEye, vec_mul(bodyRight, 0.012f)), faceVisibility);
    set_vec(MP_LeftEye, leftEye, faceVisibility);
    set_vec(MP_LeftEyeOuter, vec_add(leftEye, vec_mul(bodyRight, -0.012f)), faceVisibility);
    set_vec(MP_RightEyeInner, vec_add(rightEye, vec_mul(bodyRight, -0.012f)), faceVisibility);
    set_vec(MP_RightEye, rightEye, faceVisibility);
    set_vec(MP_RightEyeOuter, vec_add(rightEye, vec_mul(bodyRight, 0.012f)), faceVisibility);
    set_vec(MP_LeftEar, leftEar, faceVisibility);
    set_vec(MP_RightEar, rightEar, faceVisibility);
    set_vec(MP_MouthLeft, vec_add(mouthPlane, vec_mul(bodyRight, -0.025f)), faceVisibility);
    set_vec(MP_MouthRight, vec_add(mouthPlane, vec_mul(bodyRight, 0.025f)), faceVisibility);
    // arms
    set(MP_LeftShoulder,  NUI_SKELETON_POSITION_SHOULDER_LEFT,  1.0f);
    set(MP_RightShoulder, NUI_SKELETON_POSITION_SHOULDER_RIGHT, 1.0f);
    set(MP_LeftElbow,     NUI_SKELETON_POSITION_ELBOW_LEFT,     1.0f);
    set(MP_RightElbow,    NUI_SKELETON_POSITION_ELBOW_RIGHT,    1.0f);
    set(MP_LeftWrist,     NUI_SKELETON_POSITION_WRIST_LEFT,     1.0f);
    set(MP_RightWrist,    NUI_SKELETON_POSITION_WRIST_RIGHT,    1.0f);
    // NUI exposes Wrist and Hand but no finger tips. Preserve the real Hand
    // point, then extend a short HandTip and lateral Thumb. The fallback to the
    // forearm direction also prevents coincident joints on inferred frames.
    auto synth_hand = [&](bool left, int pinkyMp, int indexMp, int thumbMp,
                          int handJoint, int wristJoint, int elbowJoint) {
        const Vec3f wrist = vec_of(sk.SkeletonPositions[wristJoint]);
        Vec3f hand = vec_of(sk.SkeletonPositions[handJoint]);
        Vec3f direction = vec_normalize_or(
            vec_sub(hand, wrist),
            vec_normalize_or(vec_sub(wrist,
                                     vec_of(sk.SkeletonPositions[elbowJoint])),
                             {0.f, 1.f, 0.f}));
        if (vec_len(vec_sub(hand, wrist)) < 0.025f) {
            hand = vec_add(wrist, vec_mul(direction, 0.080f));
        }
        const float visibility = std::min(nui_vis(sk, handJoint), nui_vis(sk, wristJoint));
        set_vec(pinkyMp, hand, visibility);
        set_vec(indexMp, vec_add(hand, vec_mul(direction, 0.070f)), visibility);
        const float thumbSide = left ? 0.035f : -0.035f;
        set_vec(thumbMp,
                vec_add(vec_add(hand, vec_mul(direction, 0.025f)),
                        vec_mul(bodyRight, thumbSide)),
                visibility);
    };
    synth_hand(true, MP_LeftPinky, MP_LeftIndex, MP_LeftThumb,
               NUI_SKELETON_POSITION_HAND_LEFT,
               NUI_SKELETON_POSITION_WRIST_LEFT,
               NUI_SKELETON_POSITION_ELBOW_LEFT);
    synth_hand(false, MP_RightPinky, MP_RightIndex, MP_RightThumb,
               NUI_SKELETON_POSITION_HAND_RIGHT,
               NUI_SKELETON_POSITION_WRIST_RIGHT,
               NUI_SKELETON_POSITION_ELBOW_RIGHT);
    // legs
    set(MP_LeftHip,       NUI_SKELETON_POSITION_HIP_LEFT,       1.0f);
    set(MP_RightHip,      NUI_SKELETON_POSITION_HIP_RIGHT,      1.0f);
    set(MP_LeftKnee,      NUI_SKELETON_POSITION_KNEE_LEFT,      1.0f);
    set(MP_RightKnee,     NUI_SKELETON_POSITION_KNEE_RIGHT,     1.0f);
    set(MP_LeftAnkle,     NUI_SKELETON_POSITION_ANKLE_LEFT,     1.0f);
    set(MP_RightAnkle,    NUI_SKELETON_POSITION_ANKLE_RIGHT,    1.0f);
    set(MP_LeftHeel,      NUI_SKELETON_POSITION_ANKLE_LEFT,     1.0f);
    set(MP_RightHeel,     NUI_SKELETON_POSITION_ANKLE_RIGHT,    1.0f);
    set(MP_LeftFootIndex, NUI_SKELETON_POSITION_FOOT_LEFT,      1.0f);
    set(MP_RightFootIndex,NUI_SKELETON_POSITION_FOOT_RIGHT,     1.0f);

    const int key[] = { MP_LeftShoulder, MP_RightShoulder, MP_LeftHip, MP_RightHip,
                        MP_LeftKnee, MP_RightKnee, MP_Nose };
    float conf = 0;
    for (int k : key) conf += out->vis[k];
    out->confidence = conf / (float)(sizeof(key) / sizeof(key[0]));
    out->valid = true;
}

// ---------------------------------------------------------------- impl ----
struct KinectSource::Impl {
    explicit Impl(const KinectTrackingOptions& requested) : tracking(requested) {
        tracking.smoothing = std::clamp(tracking.smoothing, 0.0f, 1.0f);
        tracking.correction = std::clamp(tracking.correction, 0.0f, 1.0f);
        tracking.prediction = std::clamp(tracking.prediction, 0.0f, 1.0f);
        tracking.jitter_radius = std::clamp(tracking.jitter_radius, 0.0f, 1.0f);
        tracking.max_deviation_radius = std::clamp(
            tracking.max_deviation_radius, 0.0f, 1.0f);
        tracking.skeleton_lock_ms = std::clamp(tracking.skeleton_lock_ms, 0, 2000);
        tracking.body_hold_ms = std::clamp(tracking.body_hold_ms, 0, 2000);
        tracking.max_joint_speed_mps = std::clamp(
            tracking.max_joint_speed_mps, 1.0f, 20.0f);
        tracking.bone_length_tolerance = std::clamp(
            tracking.bone_length_tolerance, 0.10f, 1.00f);
        tracking.color_fps = std::clamp(tracking.color_fps, 1, 30);
    }

    KinectTrackingOptions tracking;
    NuiApi api;
    HANDLE colorStream = nullptr;
    std::atomic<bool> stop{false};
    std::thread worker;
    std::atomic<bool> haveFrame{false};
    std::atomic<bool> loggedFirstColorFrame{false};
    std::mutex mtx;
    std::vector<uint8_t> frame;        // latest color frame, native size, BGR8
    std::vector<uint8_t> writeFrame;   // capture-thread scratch/back buffer
    int width = 640, height = 480;     // NUI_IMAGE_RESOLUTION_640x480
    uint64_t colorFrameCount = 0;      // capture-worker thread only
    uint64_t lastColorCopyTick = 0;    // capture-worker thread only
    // latest skeleton (mapped), guarded by skMtx
    std::mutex skMtx;
    std::condition_variable skCv;
    PoseResult skel;
    uint64_t skGeneration = 0;
    std::atomic<bool> skelSeen{false};
    std::atomic<uint64_t> skelTick{0};
    uint64_t skeletonFrameCount = 0;   // capture-worker thread only
    bool bodyWasTracked = false;
    DWORD lastTrackingId = 0;
    DWORD activeTrackingId = 0;
    uint64_t activeTrackingTick = 0;
    bool jointFilterReady = false;
    DWORD jointFilterTrackingId = 0;
    uint64_t jointFilterTick = 0;
    Vec3f filteredJoint[nui::NUI_SKELETON_POSITION_COUNT] {};
    float learnedBoneLength[nui::NUI_SKELETON_POSITION_COUNT] {};
    uint64_t repairedJointCount = 0;
    uint64_t rejectedOutlierCount = 0;
    uint64_t skeletonLockHoldCount = 0;

    bool init() {
        if (!api.load()) return false;
        DWORD initializeFlags = nui::kInitializeDepthAndPlayer |
                                nui::kInitializeSkeleton;
        if (tracking.use_color) {
            initializeFlags |= nui::kInitializeColor;
        }
        HRESULT hr = api.NuiInitialize(initializeFlags);
        if (FAILED(hr)) { logf("NuiInitialize failed: 0x%08lx", (unsigned long)hr); return false; }
        if (tracking.use_color) {
            hr = api.NuiImageStreamOpen(nui::NUI_IMAGE_TYPE_COLOR,
                                        nui::NUI_IMAGE_RESOLUTION_640x480,
                                        0, 2, nullptr, &colorStream);
            if (FAILED(hr)) {
                logf("NuiImageStreamOpen(color 640x480) failed: 0x%08lx", (unsigned long)hr);
                api.NuiShutdown();
                return false;
            }
        }
        hr = api.NuiSkeletonTrackingEnable(nullptr, 0);
        if (FAILED(hr)) {
            logf("NuiSkeletonTrackingEnable failed: 0x%08lx", (unsigned long)hr);
            api.NuiShutdown();
            return false;
        }
        LONG elevationAngle = 0;
        const HRESULT elevationHr = api.NuiCameraElevationGetAngle(&elevationAngle);
        if (SUCCEEDED(elevationHr)) {
            logf("camera elevation=%ld degrees (read-only; motor unchanged)",
                 (long)elevationAngle);
        } else {
            logf("NuiCameraElevationGetAngle failed: 0x%08lx",
                 (unsigned long)elevationHr);
        }
        frame.assign((size_t)width * height * 3, 0);
        writeFrame.assign((size_t)width * height * 3, 0);
        logf(
            "initialized (%s), VP4U_KINECT_NO_COLOR enabled=%d; "
            "VP4U_KINECT_LOW_LATENCY smoothing=%.2f correction=%.2f "
            "prediction=%.2f jitter=%.3f maxDeviation=%.3f; "
            "VP4U_KINECT_STABLE_BODY lock=%dms hold=%dms "
            "maxJointSpeed=%.1fm/s boneTolerance=%.0f%%",
            tracking.use_color ? "color 640x480 + skeleton" : "skeleton-only",
            (int)!tracking.use_color, tracking.smoothing, tracking.correction,
            tracking.prediction, tracking.jitter_radius,
            tracking.max_deviation_radius, tracking.skeleton_lock_ms,
            tracking.body_hold_ms, tracking.max_joint_speed_mps,
            tracking.bone_length_tolerance * 100.f);
        return true;
    }

    void start() { worker = std::thread([this] { run(); }); }

    void join() {
        stop = true;
        skCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void shutdown() {
        join();
        if (api.NuiShutdown) api.NuiShutdown();
        if (api.dll) { FreeLibrary(api.dll); api.dll = nullptr; }
    }

    void copy_color_frame(const nui::NUI_IMAGE_FRAME* f) {
        nui::INuiFrameTexture* tex = f->pFrameTexture;
        if (!tex) return;
        nui::NUI_LOCKED_RECT lr;
        std::memset(&lr, 0, sizeof lr);
        if (FAILED(tex->LockRect(0, &lr, nullptr, 0)) || !lr.pBits) return;
        // color stream delivers 32bpp BGRX; convert to BGR24 top-down
        const int pitch = lr.Pitch;
        if (pitch >= width * 4 && lr.size >= pitch * height) {
            // Convert into a private back buffer.  The old implementation held
            // mtx during the full 640x480 BGRX->BGR loop, which blocked both
            // preview and pose workers and reduced the callback rate
            // to single digits under contention.
            for (int y = 0; y < height; y++) {
                const uint8_t* src = lr.pBits + (size_t)y * pitch;
                uint8_t* dst = writeFrame.data() + (size_t)y * width * 3;
                for (int x = 0; x < width; x++) {
                    dst[x * 3 + 0] = src[x * 4 + 0]; // B
                    dst[x * 3 + 1] = src[x * 4 + 1]; // G
                    dst[x * 3 + 2] = src[x * 4 + 2]; // R
                }
            }
            const bool shouldLogFirst = !loggedFirstColorFrame.load();
            uint64_t sample_sum = 0;
            size_t sample_count = 0;
            if (shouldLogFirst) {
                const size_t sample_step = std::max<size_t>(3, writeFrame.size() / 1024);
                for (size_t i = 0; i < writeFrame.size(); i += sample_step) {
                    sample_sum += writeFrame[i];
                    sample_count++;
                }
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                frame.swap(writeFrame);
            }
            haveFrame = true;
            if (!loggedFirstColorFrame.exchange(true)) {
                logf("first real color frame received (%dx%d, pitch=%d, sample mean=%llu)",
                     width, height, pitch,
                     (unsigned long long)(sample_count ? sample_sum / sample_count : 0));
            }
        }
        tex->UnlockRect(0);
    }

    const nui::NUI_SKELETON_DATA* select_skeleton(
            const nui::NUI_SKELETON_FRAME* sf, uint64_t now) {
        // Prefer the already selected NUI tracking ID. Array order is not a
        // stable identity and choosing the first TRACKED slot can swap the
        // performer with another/ghost body for a single frame.
        if (activeTrackingId != 0) {
            for (int i = 0; i < nui::kSkeletonCount; ++i) {
                const auto& candidate = sf->SkeletonData[i];
                if (candidate.eTrackingState == nui::NUI_SKELETON_TRACKED &&
                    candidate.dwTrackingID == activeTrackingId) {
                    activeTrackingTick = now;
                    return &candidate;
                }
            }
            if (activeTrackingTick != 0 &&
                now - activeTrackingTick < (uint64_t)tracking.skeleton_lock_ms) {
                ++skeletonLockHoldCount;
                return nullptr;
            }
        }

        const nui::NUI_SKELETON_DATA* best = nullptr;
        int bestScore = -1000000;
        for (int i = 0; i < nui::kSkeletonCount; ++i) {
            const auto& candidate = sf->SkeletonData[i];
            if (candidate.eTrackingState != nui::NUI_SKELETON_TRACKED) continue;
            int trackedJoints = 0;
            for (int j = 0; j < nui::NUI_SKELETON_POSITION_COUNT; ++j) {
                if (candidate.eSkeletonPositionTrackingState[j] ==
                    nui::NUI_SKELETON_POSITION_TRACKED) {
                    ++trackedJoints;
                }
            }
            int score = trackedJoints * 100 -
                        count_quality_bits(candidate.dwQualityFlags) * 25;
            if (jointFilterReady) {
                const Vec3f hip = vec_of(candidate.SkeletonPositions[
                    nui::NUI_SKELETON_POSITION_HIP_CENTER]);
                if (vec_is_valid(hip)) {
                    score -= (int)(vec_len(vec_sub(hip, filteredJoint[0])) * 100.f);
                }
            }
            if (!best || score > bestScore) {
                best = &candidate;
                bestScore = score;
            }
        }

        if (!best) {
            activeTrackingId = 0;
            jointFilterReady = false;
            return nullptr;
        }
        if (activeTrackingId != 0 && activeTrackingId != best->dwTrackingID) {
            logf("stable-body tracking ID switch %lu -> %lu after %dms lock",
                 (unsigned long)activeTrackingId,
                 (unsigned long)best->dwTrackingID,
                 tracking.skeleton_lock_ms);
        }
        activeTrackingId = best->dwTrackingID;
        activeTrackingTick = now;
        if (jointFilterTrackingId != activeTrackingId) jointFilterReady = false;
        return best;
    }

    void reset_joint_filter(nui::NUI_SKELETON_DATA* sk, uint64_t now) {
        std::memset(learnedBoneLength, 0, sizeof learnedBoneLength);
        for (int j = 0; j < nui::NUI_SKELETON_POSITION_COUNT; ++j) {
            Vec3f point = vec_of(sk->SkeletonPositions[j]);
            if (!vec_is_valid(point)) {
                const int parent = kJointParent[j];
                point = parent >= 0 ? filteredJoint[parent] : Vec3f{0.f, 0.f, 2.f};
                vec_store(&sk->SkeletonPositions[j], point);
                sk->eSkeletonPositionTrackingState[j] =
                    nui::NUI_SKELETON_POSITION_INFERRED;
                ++repairedJointCount;
            }
            filteredJoint[j] = point;
            const int parent = kJointParent[j];
            if (parent >= 0 &&
                sk->eSkeletonPositionTrackingState[j] ==
                    nui::NUI_SKELETON_POSITION_TRACKED &&
                sk->eSkeletonPositionTrackingState[parent] ==
                    nui::NUI_SKELETON_POSITION_TRACKED) {
                const float length = vec_len(vec_sub(point, filteredJoint[parent]));
                if (length >= 0.03f && length <= 1.20f) {
                    learnedBoneLength[j] = length;
                }
            }
        }
        jointFilterReady = true;
        jointFilterTrackingId = sk->dwTrackingID;
        jointFilterTick = now;
    }

    void stabilize_skeleton(nui::NUI_SKELETON_DATA* sk, uint64_t now) {
        if (!jointFilterReady || jointFilterTrackingId != sk->dwTrackingID ||
            jointFilterTick == 0 || now - jointFilterTick > 1000) {
            reset_joint_filter(sk, now);
            return;
        }

        Vec3f previous[nui::NUI_SKELETON_POSITION_COUNT];
        Vec3f raw[nui::NUI_SKELETON_POSITION_COUNT];
        nui::NUI_SKELETON_POSITION_TRACKING_STATE
            rawState[nui::NUI_SKELETON_POSITION_COUNT];
        std::memcpy(previous, filteredJoint, sizeof previous);
        for (int j = 0; j < nui::NUI_SKELETON_POSITION_COUNT; ++j) {
            raw[j] = vec_of(sk->SkeletonPositions[j]);
            rawState[j] = sk->eSkeletonPositionTrackingState[j];
        }

        const float dt = std::clamp((float)(now - jointFilterTick) / 1000.f,
                                    0.010f, 0.150f);
        for (int j = 0; j < nui::NUI_SKELETON_POSITION_COUNT; ++j) {
            const int parent = kJointParent[j];
            const bool reallyTracked =
                rawState[j] == nui::NUI_SKELETON_POSITION_TRACKED &&
                vec_is_valid(raw[j]);
            Vec3f predicted = previous[j];
            if (parent >= 0) {
                predicted = vec_add(previous[j],
                    vec_sub(filteredJoint[parent], previous[parent]));
            }

            Vec3f candidate = predicted;
            if (reallyTracked) {
                candidate = raw[j];
                // Work in parent-relative motion, so fast whole-body movement
                // is retained while an isolated endpoint explosion is capped.
                float speedScale = 1.0f;
                if (j == nui::NUI_SKELETON_POSITION_HIP_CENTER) speedScale = 0.50f;
                else if (j == nui::NUI_SKELETON_POSITION_SPINE ||
                         j == nui::NUI_SKELETON_POSITION_SHOULDER_CENTER ||
                         j == nui::NUI_SKELETON_POSITION_HEAD ||
                         j == nui::NUI_SKELETON_POSITION_SHOULDER_LEFT ||
                         j == nui::NUI_SKELETON_POSITION_SHOULDER_RIGHT ||
                         j == nui::NUI_SKELETON_POSITION_HIP_LEFT ||
                         j == nui::NUI_SKELETON_POSITION_HIP_RIGHT) {
                    speedScale = 0.65f;
                }
                bool limited = false;
                candidate = vec_limit_delta(
                    predicted, candidate,
                    tracking.max_joint_speed_mps * speedScale * dt, &limited);
                if (limited) ++rejectedOutlierCount;
            } else {
                // Never integrate NUI's inferred endpoint. Preserve the last
                // local limb vector and carry it with its filtered parent.
                sk->eSkeletonPositionTrackingState[j] =
                    nui::NUI_SKELETON_POSITION_INFERRED;
                ++repairedJointCount;
            }

            if (parent >= 0) {
                Vec3f segment = vec_sub(candidate, filteredJoint[parent]);
                float segmentLength = vec_len(segment);
                const float expected = learnedBoneLength[j];
                if (expected > 0.02f && segmentLength > 1.0e-5f) {
                    const float minLength = expected *
                        (1.f - tracking.bone_length_tolerance);
                    const float maxLength = expected *
                        (1.f + tracking.bone_length_tolerance);
                    const float clampedLength = std::clamp(
                        segmentLength, minLength, maxLength);
                    if (std::fabs(clampedLength - segmentLength) > 1.0e-5f) {
                        candidate = vec_add(filteredJoint[parent],
                            vec_mul(segment, clampedLength / segmentLength));
                        segmentLength = clampedLength;
                        ++rejectedOutlierCount;
                    }
                }

                // Learn bone lengths only from fully tracked raw endpoints and
                // never let one bad sample redefine the performer's skeleton.
                if (reallyTracked &&
                    rawState[parent] == nui::NUI_SKELETON_POSITION_TRACKED &&
                    vec_is_valid(raw[parent])) {
                    const float rawLength = vec_len(vec_sub(raw[j], raw[parent]));
                    if (rawLength >= 0.03f && rawLength <= 1.20f) {
                        float& learned = learnedBoneLength[j];
                        if (learned <= 0.02f) {
                            learned = rawLength;
                        } else {
                            const float low = learned *
                                (1.f - tracking.bone_length_tolerance);
                            const float high = learned *
                                (1.f + tracking.bone_length_tolerance);
                            if (rawLength >= low && rawLength <= high) {
                                learned = learned * 0.99f + rawLength * 0.01f;
                            }
                        }
                    }
                }
            }

            filteredJoint[j] = candidate;
            vec_store(&sk->SkeletonPositions[j], candidate);
        }
        jointFilterTick = now;
    }

    void on_skeleton_frame(nui::NUI_SKELETON_FRAME* sf) {
        const nui::NUI_TRANSFORM_SMOOTH_PARAMETERS smooth = {
            tracking.smoothing,
            tracking.correction,
            tracking.prediction,
            tracking.jitter_radius,
            tracking.max_deviation_radius
        };
        api.NuiTransformSmooth(sf, &smooth); // best-effort jitter reduction
        skeletonFrameCount++;
        const uint64_t now = GetTickCount64();
        if (skeletonFrameCount == 1) {
            logf("first skeleton frame received (NUI skeleton stream active)");
        }

        nui::NUI_SKELETON_DATA stableSkeleton {};
        const nui::NUI_SKELETON_DATA* selected = select_skeleton(sf, now);
        if (selected) {
            stableSkeleton = *selected;
            stabilize_skeleton(&stableSkeleton, now);
        }
        const nui::NUI_SKELETON_DATA* tracked = selected ? &stableSkeleton : nullptr;

        const bool bodyTracked = tracked != nullptr;
        const DWORD trackingId = tracked ? tracked->dwTrackingID : 0;
        const bool trackingChanged = bodyTracked != bodyWasTracked ||
                                     (bodyTracked && trackingId != lastTrackingId);
        if (trackingChanged) {
            if (tracked) {
                const nui::Vector4& hip = tracked->SkeletonPositions[
                    nui::NUI_SKELETON_POSITION_HIP_CENTER];
                char clipFlags[64];
                format_clip_flags(tracked->dwQualityFlags, clipFlags,
                                  sizeof clipFlags);
                logf(
                    "body TRACKED id=%lu hip(camera)=(%.3f, %.3f, %.3f) "
                    "quality=0x%08lx clip=%s "
                    "stable(repaired=%llu rejected=%llu idHold=%llu)",
                    (unsigned long) trackingId,
                    hip.x, hip.y, hip.z,
                    (unsigned long) tracked->dwQualityFlags,
                    clipFlags,
                    (unsigned long long)repairedJointCount,
                    (unsigned long long)rejectedOutlierCount,
                    (unsigned long long)skeletonLockHoldCount);
            } else {
                logf(
                    "body NOT TRACKED (skeleton stream healthy, activeId=%lu)",
                    (unsigned long)activeTrackingId);
            }
        }
        bodyWasTracked = bodyTracked;
        lastTrackingId = trackingId;

        PoseResult pr; // valid=false when nobody is tracked
        if (tracked) fill_pose(*tracked, &pr);
        {
            std::lock_guard<std::mutex> lk(skMtx);
            skel = pr;
            ++skGeneration;
        }
        skelSeen = true;
        skelTick = now;
        skCv.notify_all();
    }

    void run() {
        int err_log = 0;
        while (!stop) {
            if (!tracking.use_color) {
                // Skeleton tracking has its own 30 Hz stream; blocking on it
                // removes all RGB acquisition/conversion and gives the pose
                // callback an exact hardware-frame clock.
                nui::NUI_SKELETON_FRAME sf;
                const HRESULT hr = api.NuiSkeletonGetNextFrame(100, &sf);
                if (SUCCEEDED(hr)) {
                    on_skeleton_frame(&sf);
                } else if (FAILED(hr) && hr != E_NUI_FRAME_NO_DATA) {
                    if (++err_log % 50 == 1)
                        logf("NuiSkeletonGetNextFrame: 0x%08lx", (unsigned long)hr);
                }
                continue;
            }

            // color (short wait paces the loop at ~color rate)
            const nui::NUI_IMAGE_FRAME* f = nullptr;
            HRESULT hr = api.NuiImageStreamGetNextFrame(colorStream, 100, &f);
            if (SUCCEEDED(hr) && f) {
                ++colorFrameCount;
                const uint64_t now = GetTickCount64();
                const uint64_t colorInterval = (uint64_t)std::max(
                    1, 1000 / tracking.color_fps);
                // NUI color acquisition still paces skeleton polling at 30 Hz,
                // but pixel conversion only needs to match the preview rate.
                if (!haveFrame || now - lastColorCopyTick >= colorInterval) {
                    copy_color_frame(f);
                    lastColorCopyTick = now;
                }
                api.NuiImageStreamReleaseFrame(colorStream, f);
            } else if (FAILED(hr) && hr != E_NUI_FRAME_NO_DATA) {
                if (++err_log % 50 == 1)
                    logf("NuiImageStreamGetNextFrame: 0x%08lx", (unsigned long)hr);
            }
            // skeleton (non-blocking drain, one per color frame)
            nui::NUI_SKELETON_FRAME sf;
            hr = api.NuiSkeletonGetNextFrame(0, &sf);
            if (SUCCEEDED(hr)) on_skeleton_frame(&sf);
        }
    }
};

// ---- singleton ----
static std::mutex g_kinMtx;
static std::weak_ptr<KinectSource> g_kinInstance;

std::shared_ptr<KinectSource> KinectSource::acquire(
        const KinectTrackingOptions& options) {
    std::lock_guard<std::mutex> lk(g_kinMtx);
    if (auto sp = g_kinInstance.lock()) return sp;
    auto sp = std::shared_ptr<KinectSource>(new KinectSource(options));
    if (!sp->impl_->init()) return nullptr; // details already on stderr
    sp->impl_->start();
    g_kinInstance = sp;
    return sp;
}

KinectSource::KinectSource(const KinectTrackingOptions& options)
    : impl_(new Impl(options)) {}
KinectSource::~KinectSource() {
    impl_->shutdown();
    delete impl_;
}

bool KinectSource::is_real_camera() const { return impl_->haveFrame; }
bool KinectSource::has_color_stream() const { return impl_->tracking.use_color; }
int KinectSource::native_width() const { return impl_->width; }
int KinectSource::native_height() const { return impl_->height; }

bool KinectSource::get_frame_bgr8(std::vector<uint8_t>& out, int w, int h) {
    // Copy the native frame under the lock, scale after releasing it: the
    // bilinear scale is ~9ms at 848x480 and must not block the capture worker
    // (or the other reader thread) on impl_->mtx.
    static thread_local std::vector<uint8_t> native;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->haveFrame) {
            if (impl_->width == w && impl_->height == h) {
                out = impl_->frame;
                return true;
            }
            native = impl_->frame;
            have = true;
        }
    }
    if (have) {
        out.resize((size_t)w * h * 3);
        scale_bgr8_bilinear(native.data(), impl_->width, impl_->height,
                            out.data(), w, h);
        return true;
    }
    out.resize((size_t)w * h * 3);
    double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    make_synthetic_bgr8(out, w, h, t, 2);
    return false;
}

bool KinectSource::get_skeleton(PoseResult* out) {
    if (!impl_->skelSeen) return false;
    if (GetTickCount64() - impl_->skelTick.load() > 2000) return false; // stream stalled
    std::lock_guard<std::mutex> lk(impl_->skMtx);
    *out = impl_->skel;
    return true;
}

bool KinectSource::wait_for_skeleton(PoseResult* out, uint64_t* generation,
                                     int timeout_ms) {
    if (!out || !generation) return false;
    timeout_ms = std::clamp(timeout_ms, 1, 2000);
    std::unique_lock<std::mutex> lk(impl_->skMtx);
    const bool woke = impl_->skCv.wait_for(
        lk, std::chrono::milliseconds(timeout_ms), [&] {
            return impl_->stop.load() || impl_->skGeneration != *generation;
        });
    if (!woke || impl_->skGeneration == *generation) return false;
    *generation = impl_->skGeneration;
    *out = impl_->skel;
    return GetTickCount64() - impl_->skelTick.load() <= 2000;
}

} // namespace vp4u
