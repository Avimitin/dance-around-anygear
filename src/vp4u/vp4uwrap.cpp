// VP4U compatibility implementation for external pose sensors.
#include "vp4u_abi.h"
#include "webcam.h"
#include "pose.h"
#include "kinect.h"
#include "steamvr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace vp4u {

// ---------------------------------------------------------------- logger ----
struct LoggerEntry { OnLogCallback cb; void* ctx; };
static std::mutex g_logMtx;
static std::vector<LoggerEntry> g_loggers; // handle = index+1; 0 = invalid

static intptr_t logger_add(OnLogCallback cb, void* ctx) {
    if (!cb) return 0;
    std::lock_guard<std::mutex> lk(g_logMtx);
    for (size_t i = 0; i < g_loggers.size(); i++) {
        if (g_loggers[i].cb == cb && g_loggers[i].ctx == ctx) {
            return (intptr_t)(i + 1);
        }
    }
    for (size_t i = 0; i < g_loggers.size(); i++) {
        if (!g_loggers[i].cb) { g_loggers[i] = {cb, ctx}; return (intptr_t)(i + 1); }
    }
    g_loggers.push_back({cb, ctx});
    return (intptr_t)g_loggers.size();
}

static void logger_remove(intptr_t h) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (h >= 1 && (size_t)h <= g_loggers.size()) g_loggers[(size_t)h - 1] = {nullptr, nullptr};
}

static void vlog(const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    std::lock_guard<std::mutex> lk(g_logMtx);
    for (auto& e : g_loggers) {
        if (e.cb) e.cb(e.ctx, buf);
    }
}

static void log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- config ----
struct Config {
#if defined(ANYGEAR_BACKEND_KINECT)
    int capture_w = 848, capture_h = 480, capture_fps = 30;
#else
    int capture_w = 848, capture_h = 480, capture_fps = 30;
#endif
    // Vp4uWrap custom section (all optional)
    int   camera_index = 0;
#if defined(ANYGEAR_BACKEND_KINECT)
    int   preview_fps = 15;
    int   image_pool_max = 12;
#else
    int   preview_fps = 15;
    int   image_pool_max = 12;
#endif
    bool  mirror_x = false;
    // Kinect v1 NUI skeleton space is right-handed (+Y up, +Z away from
    // the sensor), which makes +X point to camera-left. The target stage uses
    // +X camera-right, so the Kinect backend applies a horizontal conversion.
    bool  kinect_flip_x = true;
    // The performer and on-screen avatar face each other while movement stays
    // mirrored in screen space. Rotate around the configured horizontal center
    // and around the live hip in depth.
    bool  kinect_face_to_face = true;
    float kinect_mirror_center_x = 0.55f;
    bool  kinect_use_color = false;
    // The callback contract requires a non-null main image with valid 3D
    // bodies. False uses a 1x1 sentinel rather than copying two
    // 640x480 images into every hardware-skeleton callback.
    bool  kinect_pose_images = false;
    float kinect_offset_x = 0.0f;
    float kinect_offset_y = 0.0f;
    float kinect_offset_z = 0.0f;
    float kinect_smoothing = 0.35f;
    float kinect_correction = 0.65f;
    float kinect_prediction = 0.00f;
    float kinect_jitter_radius = 0.05f;
    float kinect_max_deviation_radius = 0.04f;
    int   kinect_skeleton_lock_ms = 400;
    int   kinect_body_hold_ms = 500;
    float kinect_max_joint_speed_mps = 8.0f;
    float kinect_bone_length_tolerance = 0.35f;
    bool  steamvr_auto_center = true;
    bool  steamvr_face_to_face = true;
    bool  steamvr_require_waist = true;
    bool  steamvr_require_feet = true;
    int   steamvr_body_hold_ms = 250;
    float steamvr_stage_hip_x = 0.55f;
    float steamvr_stage_hip_y = 0.0f;
    float steamvr_stage_hip_z = 2.30f;
    std::string steamvr_runtime_dir;
    float hip_stage_x = 0.9f, hip_stage_y = 1.0f, hip_stage_z = 0.75f;
    float torso_len_m = 0.52f;
    float z_damping = 0.7f;
    float ema_alpha = 0.55f;
    std::string model_path;   // default: <dll>/dance_around_anygear_webcam/*.task
    std::string runtime_dir;  // default: <dll>/dance_around_anygear_webcam
};

static Config g_cfg;
static std::string g_cfgPath;

static void config_apply_environment() {
    const auto apply_int = [](const char *name, int min_value, int max_value, int *target) {
        const char *text = std::getenv(name);
        if (!text || !*text) return;
        char *end = nullptr;
        const long value = std::strtol(text, &end, 10);
        if (end == text || *end != '\0' || value < min_value || value > max_value) {
            log("config: ignoring invalid %s='%s' (expected %d..%d)",
                name, text, min_value, max_value);
            return;
        }
        *target = (int)value;
    };

#if defined(ANYGEAR_BACKEND_WEBCAM)
    apply_int("VP4U_CAPTURE_FPS", 1, 30, &g_cfg.capture_fps);
#else
    apply_int("VP4U_CAPTURE_FPS", 1, 60, &g_cfg.capture_fps);
#endif
    apply_int("VP4U_PREVIEW_FPS", 1, 30, &g_cfg.preview_fps);
    apply_int("VP4U_IMAGE_POOL_MAX", 4, 24, &g_cfg.image_pool_max);
    apply_int("VP4U_CAMERA_INDEX", 0, 31, &g_cfg.camera_index);
}

// tolerant extraction: find "key" then parse the number/colon value after it
static bool json_find_number(const std::string& js, const char* key, double* out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return false;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\r' || js[p] == '\n')) p++;
    char* end = nullptr;
    double v = std::strtod(js.c_str() + p, &end);
    if (end == js.c_str() + p) return false;
    *out = v;
    return true;
}

static bool json_find_string(const std::string& js, const char* key, std::string* out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return false;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p = js.find('"', p + 1);
    if (p == std::string::npos) return false;
    size_t e = js.find('"', p + 1);
    if (e == std::string::npos) return false;
    *out = js.substr(p + 1, e - p - 1);
    return true;
}

static void config_apply_json(const std::string& js) {
    double v;
    if (json_find_number(js, "CaptureWidth", &v)) g_cfg.capture_w = (int)v;
    if (json_find_number(js, "CaptureHeight", &v)) g_cfg.capture_h = (int)v;
    if (json_find_number(js, "CaptureFrameRate", &v)) g_cfg.capture_fps = (int)v;
    // custom keys must live inside our own "Vp4uWrap" section so that e.g.
    // VisionPoseConfig.ModelPath of the real config is not picked up.
    std::string pat = "\"Vp4uWrap\"";
    size_t sec = js.find(pat);
    if (sec == std::string::npos) return;
    std::string sub = js.substr(sec + pat.size());
    if (json_find_number(sub, "CameraIndex", &v)) g_cfg.camera_index = (int)v;
    if (json_find_number(sub, "PreviewFps", &v)) g_cfg.preview_fps = (int)v;
    if (json_find_number(sub, "ImagePoolMax", &v)) g_cfg.image_pool_max = (int)v;
    if (json_find_number(sub, "HipStageX", &v)) g_cfg.hip_stage_x = (float)v;
    if (json_find_number(sub, "HipStageY", &v)) g_cfg.hip_stage_y = (float)v;
    if (json_find_number(sub, "HipStageZ", &v)) g_cfg.hip_stage_z = (float)v;
    if (json_find_number(sub, "TorsoLenM", &v)) g_cfg.torso_len_m = (float)v;
    if (json_find_number(sub, "ZDamping", &v)) g_cfg.z_damping = (float)v;
    if (json_find_number(sub, "EmaAlpha", &v)) g_cfg.ema_alpha = (float)v;
    if (json_find_number(sub, "KinectFlipX", &v)) g_cfg.kinect_flip_x = (v != 0);
    if (json_find_number(sub, "KinectFaceToFace", &v)) g_cfg.kinect_face_to_face = (v != 0);
    if (json_find_number(sub, "KinectMirrorCenterX", &v)) g_cfg.kinect_mirror_center_x = (float)v;
    if (json_find_number(sub, "KinectUseColor", &v)) g_cfg.kinect_use_color = (v != 0);
    if (json_find_number(sub, "KinectPoseImages", &v)) g_cfg.kinect_pose_images = (v != 0);
    if (json_find_number(sub, "KinectOffsetX", &v)) g_cfg.kinect_offset_x = (float)v;
    if (json_find_number(sub, "KinectOffsetY", &v)) g_cfg.kinect_offset_y = (float)v;
    if (json_find_number(sub, "KinectOffsetZ", &v)) g_cfg.kinect_offset_z = (float)v;
    if (json_find_number(sub, "KinectSmoothing", &v)) g_cfg.kinect_smoothing = (float)v;
    if (json_find_number(sub, "KinectCorrection", &v)) g_cfg.kinect_correction = (float)v;
    if (json_find_number(sub, "KinectPrediction", &v)) g_cfg.kinect_prediction = (float)v;
    if (json_find_number(sub, "KinectJitterRadius", &v)) g_cfg.kinect_jitter_radius = (float)v;
    if (json_find_number(sub, "KinectMaxDeviationRadius", &v)) g_cfg.kinect_max_deviation_radius = (float)v;
    if (json_find_number(sub, "KinectSkeletonLockMs", &v)) g_cfg.kinect_skeleton_lock_ms = (int)v;
    if (json_find_number(sub, "KinectBodyHoldMs", &v)) g_cfg.kinect_body_hold_ms = (int)v;
    if (json_find_number(sub, "KinectMaxJointSpeedMps", &v)) g_cfg.kinect_max_joint_speed_mps = (float)v;
    if (json_find_number(sub, "KinectBoneLengthTolerance", &v)) g_cfg.kinect_bone_length_tolerance = (float)v;
    if (json_find_number(sub, "SteamVrAutoCenter", &v)) g_cfg.steamvr_auto_center = (v != 0);
    if (json_find_number(sub, "SteamVrFaceToFace", &v)) g_cfg.steamvr_face_to_face = (v != 0);
    if (json_find_number(sub, "SteamVrRequireWaist", &v)) g_cfg.steamvr_require_waist = (v != 0);
    if (json_find_number(sub, "SteamVrRequireFeet", &v)) g_cfg.steamvr_require_feet = (v != 0);
    if (json_find_number(sub, "SteamVrBodyHoldMs", &v)) g_cfg.steamvr_body_hold_ms = (int)v;
    if (json_find_number(sub, "SteamVrStageHipX", &v)) g_cfg.steamvr_stage_hip_x = (float)v;
    if (json_find_number(sub, "SteamVrStageHipY", &v)) g_cfg.steamvr_stage_hip_y = (float)v;
    if (json_find_number(sub, "SteamVrStageHipZ", &v)) g_cfg.steamvr_stage_hip_z = (float)v;
    std::string s;
    if (json_find_string(sub, "ModelPath", &s)) g_cfg.model_path = s;
    if (json_find_string(sub, "RuntimeDir", &s)) g_cfg.runtime_dir = s;
    if (json_find_string(sub, "SteamVrRuntimeDir", &s)) g_cfg.steamvr_runtime_dir = s;
    if (json_find_number(sub, "MirrorX", &v)) g_cfg.mirror_x = (v != 0);
}

static void config_load_file(const char* path) {
    if (!path || !*path) {
        config_apply_environment();
        log("LoadConfig: empty path, using defaults/environment");
        return;
    }
    g_cfgPath = path;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        config_apply_environment();
        log("LoadConfig: cannot open '%s', using defaults/environment", path);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string js;
    if (n > 0 && n < 16 * 1024 * 1024) {
        js.resize((size_t)n);
        std::fread(&js[0], 1, (size_t)n, f);
    }
    std::fclose(f);
    config_apply_json(js);
#if defined(ANYGEAR_BACKEND_KINECT)
    // Release-safe Kinect pacing belongs to the backend target, not to a user
    // launch script or the vendor JSON. Maintainers can still override these
    // three bounded values with the documented environment variables.
    g_cfg.capture_fps = 30;
    g_cfg.preview_fps = 15;
    g_cfg.image_pool_max = 12;
#endif
    config_apply_environment();
#if defined(ANYGEAR_BACKEND_WEBCAM)
    g_cfg.capture_fps = std::clamp(g_cfg.capture_fps, 1, 30);
#else
    g_cfg.capture_fps = std::clamp(g_cfg.capture_fps, 1, 60);
#endif
    g_cfg.preview_fps = std::clamp(g_cfg.preview_fps, 1, 30);
    g_cfg.image_pool_max = std::clamp(g_cfg.image_pool_max, 4, 24);
#if defined(ANYGEAR_BACKEND_WEBCAM)
    // The vendor JSON may request the native D435 image size. Monocular pose
    // does not benefit from submitting more than the validated webcam frame,
    // so keep inference/callback allocations bounded inside the DLL.
    g_cfg.capture_w = std::clamp(g_cfg.capture_w, 160, 848);
    g_cfg.capture_h = std::clamp(g_cfg.capture_h, 120, 480);
#endif
    g_cfg.kinect_smoothing = std::clamp(g_cfg.kinect_smoothing, 0.0f, 1.0f);
    g_cfg.kinect_correction = std::clamp(g_cfg.kinect_correction, 0.0f, 1.0f);
    g_cfg.kinect_prediction = std::clamp(g_cfg.kinect_prediction, 0.0f, 1.0f);
    g_cfg.kinect_jitter_radius = std::clamp(g_cfg.kinect_jitter_radius, 0.0f, 1.0f);
    g_cfg.kinect_max_deviation_radius = std::clamp(
        g_cfg.kinect_max_deviation_radius, 0.0f, 1.0f);
    g_cfg.kinect_skeleton_lock_ms = std::clamp(g_cfg.kinect_skeleton_lock_ms, 0, 2000);
    g_cfg.kinect_body_hold_ms = std::clamp(g_cfg.kinect_body_hold_ms, 0, 2000);
    g_cfg.kinect_max_joint_speed_mps = std::clamp(
        g_cfg.kinect_max_joint_speed_mps, 1.0f, 20.0f);
    g_cfg.kinect_bone_length_tolerance = std::clamp(
        g_cfg.kinect_bone_length_tolerance, 0.10f, 1.00f);
    g_cfg.steamvr_body_hold_ms = std::clamp(
        g_cfg.steamvr_body_hold_ms, 0, 2000);
#if defined(ANYGEAR_BACKEND_KINECT)
    log("runtime config: Kinect stable profile %d Hz, preview %d Hz, pool %d, "
        "no-color=%d, face-to-face=%d, ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.preview_fps, g_cfg.image_pool_max,
        (int)!g_cfg.kinect_use_color, (int)g_cfg.kinect_face_to_face,
        sizeof(ParsedBody));
#elif defined(ANYGEAR_BACKEND_STEAMVR)
    log("runtime config: SteamVR tracked-pose profile %d Hz, preview %d Hz, "
        "pool %d, strict-waist=%d, strict-feet=%d, face-to-face=%d, "
        "ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.preview_fps, g_cfg.image_pool_max,
        (int)g_cfg.steamvr_require_waist, (int)g_cfg.steamvr_require_feet,
        (int)g_cfg.steamvr_face_to_face, sizeof(ParsedBody));
#else
    log("runtime config: MediaPipe webcam profile %d Hz, preview %d Hz, "
        "pool %d, camera=%d, frame=%dx%d, ParsedBody=%zu bytes",
        g_cfg.capture_fps, g_cfg.preview_fps, g_cfg.image_pool_max,
        g_cfg.camera_index, g_cfg.capture_w, g_cfg.capture_h,
        sizeof(ParsedBody));
#endif
}

// ------------------------------------------------------------ ImageDesc ----
// Intrusive ref-counted block: [refcount | ImageDesc | pixels...].
// The ImageDesc sits at offset of member `desc`. The callback receives one
// owned reference and releases it when finished. Cap outstanding blocks and
// drop a frame when the consumer is backlogged.
struct ImgBlock {
    std::atomic<long> ref;
    bool persistent;
    ImageDesc desc;
};

static std::mutex g_imgPoolMtx;
static std::vector<ImgBlock*> g_imgPool;   // free blocks kept for reuse
static std::atomic<long> g_imgAllocated{0}; // all allocated blocks, including the free list
static const size_t kImgPoolKeep = 8;      // free blocks retained

static ImageDesc* make_image_desc(const uint8_t* bgr, int w, int h) {
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    size_t bytes = (size_t)w * h * 3;
    ImgBlock* blk = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_imgPoolMtx);
        for (size_t i = 0; i < g_imgPool.size(); i++) {
            if (static_cast<size_t>(g_imgPool[i]->desc.BytesPerFrame) == bytes) {
                blk = g_imgPool[i];
                g_imgPool.erase(g_imgPool.begin() + (ptrdiff_t)i);
                break;
            }
        }
    }
    if (!blk) {
        long allocated = g_imgAllocated.load(std::memory_order_relaxed);
        do {
            if (allocated >= g_cfg.image_pool_max)
                return nullptr; // callback consumer backlogged: drop this frame
        } while (!g_imgAllocated.compare_exchange_weak(
            allocated, allocated + 1, std::memory_order_acq_rel));
        blk = (ImgBlock*)std::malloc(sizeof(ImgBlock) + bytes);
        if (!blk) {
            g_imgAllocated.fetch_sub(1, std::memory_order_acq_rel);
            return nullptr;
        }
    }
    blk->ref.store(1, std::memory_order_relaxed);
    blk->persistent = false;
    uint8_t* pix = (uint8_t*)(blk + 1);
    if (bgr) std::memcpy(pix, bgr, bytes);
    else std::memset(pix, 0, bytes);
    blk->desc.PointerToFrame = pix;
    blk->desc.Width = w;
    blk->desc.Height = h;
    blk->desc.BitsPerPixel = 24;
    blk->desc.BytesPerPixel = 3;
    blk->desc.BytesPerRow = w * 3;
    blk->desc.BytesPerFrame = (int)bytes;
    return &blk->desc;
}

static ImgBlock* img_block_of(ImageDesc* d) {
    return (ImgBlock*)((char*)d - offsetof(ImgBlock, desc));
}

// Kinect is a live source and the callback dispatcher can discard callbacks
// without releasing their ImageDesc.  Four process-lifetime slots (analysis
// main/sub + preview left/right) keep real camera pixels flowing without
// defeating the bounded ordinary pool. The callback consumer may retain a slot;
// seeing a newer live frame in that case is safe and preferable to a permanent
// black preview after the first few callbacks.
static std::mutex g_reusableImageMtx;
static std::array<ImgBlock*, 4> g_reusableImages {};

static ImageDesc* make_reusable_image_desc(size_t slot, const uint8_t* bgr, int w, int h) {
    if (slot >= g_reusableImages.size()) return nullptr;
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    const size_t bytes = (size_t)w * h * 3;

    std::lock_guard<std::mutex> lk(g_reusableImageMtx);
    ImgBlock* blk = g_reusableImages[slot];
    if (!blk || static_cast<size_t>(blk->desc.BytesPerFrame) != bytes) {
        // Do not free an old differently-sized slot: a callback may still own
        // its pointer. Kinect v1 is fixed at 640x480, so this is only a
        // defensive path for a future backend change.
        blk = (ImgBlock*)std::malloc(sizeof(ImgBlock) + bytes);
        if (!blk) return nullptr;
        blk->persistent = true;
        g_reusableImages[slot] = blk;
        log("kinect image slot %llu allocated (%dx%d, %llu bytes)",
            (unsigned long long)slot, w, h, (unsigned long long)bytes);
    }

    blk->ref.store(1, std::memory_order_relaxed);
    uint8_t* pix = (uint8_t*)(blk + 1);
    if (bgr) std::memcpy(pix, bgr, bytes);
    else std::memset(pix, 0, bytes);
    blk->desc.PointerToFrame = pix;
    blk->desc.Width = w;
    blk->desc.Height = h;
    blk->desc.BitsPerPixel = 24;
    blk->desc.BytesPerPixel = 3;
    blk->desc.BytesPerRow = w * 3;
    blk->desc.BytesPerFrame = (int)bytes;
    return &blk->desc;
}

// ------------------------------------------------------------ timestamp ----
static uint64_t qpc_now_us() {
    static const double freq_inv = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        return 1000000.0 / (double)f.QuadPart;
    }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * freq_inv);
}

// ------------------------------------------------------------ state ----
static std::atomic<bool> g_initialized{false};
static std::shared_ptr<CameraSource> g_cam;
static PoseEngine g_pose;
static std::atomic<bool> g_poseOk{false};

// ------------------------------------------------------------ backend ----
// Release targets select one sensor at compile time. An explicitly selected
// backend never falls through to another physical device on failure.
enum class Backend { Webcam, Kinect, SteamVr };

static Backend backend_from_env() {
#if defined(ANYGEAR_BACKEND_KINECT)
    return Backend::Kinect;
#elif defined(ANYGEAR_BACKEND_WEBCAM)
    return Backend::Webcam;
#elif defined(ANYGEAR_BACKEND_STEAMVR)
    return Backend::SteamVr;
#else
    const char* e = std::getenv("VP4U_BACKEND");
    if (!e) return Backend::Webcam;
    std::string v(e);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    if (v == "kinect") return Backend::Kinect;
    if (v == "steamvr") return Backend::SteamVr;
    return Backend::Webcam;
#endif
}

static Backend backend() {
    static const Backend b = backend_from_env();
    return b;
}

// Non-null while the corresponding hardware-skeleton backend is running.
static std::shared_ptr<KinectSource> g_kinect;
static std::shared_ptr<SteamVrSource> g_steamvr;

static bool hardware_skeleton_active() {
    return g_kinect != nullptr || g_steamvr != nullptr;
}

static bool hardware_has_color_stream() {
    return g_kinect && g_kinect->has_color_stream();
}

static bool hardware_wait_for_skeleton(PoseResult* out, uint64_t* generation,
                                       int timeout_ms) {
    if (g_kinect) {
        return g_kinect->wait_for_skeleton(out, generation, timeout_ms);
    }
    if (g_steamvr) {
        return g_steamvr->wait_for_skeleton(out, generation, timeout_ms);
    }
    return false;
}

static const char* hardware_name() {
    return g_kinect ? "Kinect" : (g_steamvr ? "SteamVR" : "hardware");
}

static int hardware_body_hold_ms() {
    return g_kinect ? g_cfg.kinect_body_hold_ms : g_cfg.steamvr_body_hold_ms;
}

// Unified frame-source access for the worker loops.
static int src_native_width() {
    if (g_kinect) return g_kinect->native_width();
    if (g_steamvr) return g_steamvr->native_width();
    return g_cam ? g_cam->native_width() : 0;
}
static int src_native_height() {
    if (g_kinect) return g_kinect->native_height();
    if (g_steamvr) return g_steamvr->native_height();
    return g_cam ? g_cam->native_height() : 0;
}
static void src_get_frame(std::vector<uint8_t>& out, int w, int h) {
    if (g_kinect) g_kinect->get_frame_bgr8(out, w, h);
    else if (g_steamvr) g_steamvr->get_frame_bgr8(out, w, h);
    else if (g_cam) g_cam->get_frame_bgr8(out, w, h);
}

struct Worker {
    std::atomic<bool> stop{false};
    std::thread th;
    void join() {
        stop = true;
        if (th.joinable()) th.join();
    }
    ~Worker() { join(); }
};

static Worker g_previewWk, g_analysisWk;

static void sleep_worker_until(Worker& worker,
                               std::chrono::steady_clock::time_point deadline) {
    const auto quantum = std::chrono::milliseconds(4);
    while (!worker.stop) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        const auto remaining = deadline - now;
        std::this_thread::sleep_for(remaining > quantum ? quantum : remaining);
    }
}

static std::string dll_dir_w() {
    static const char module_anchor = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_anchor),
            &module)) {
        return ".";
    }
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return ".";
    }
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return std::string(p.begin(), slash == std::wstring::npos ? p.begin() : p.begin() + (ptrdiff_t)slash);
}

static void paths_default(std::wstring* runtime_dir, std::string* model_path) {
    std::string dir = dll_dir_w();
    const std::string dependency_dir =
        dir + "\\dance_around_anygear_webcam";
    *runtime_dir = g_cfg.runtime_dir.empty()
        ? std::wstring(dependency_dir.begin(), dependency_dir.end())
        : std::wstring(g_cfg.runtime_dir.begin(), g_cfg.runtime_dir.end());
    *model_path = g_cfg.model_path.empty()
        ? dependency_dir + "\\pose_landmarker_lite.task"
        : g_cfg.model_path;
}

static std::wstring steamvr_runtime_default() {
    const std::string directory = g_cfg.steamvr_runtime_dir.empty()
        ? dll_dir_w() + "\\dance_around_anygear_steamvr"
        : g_cfg.steamvr_runtime_dir;
    return std::wstring(directory.begin(), directory.end());
}

// ------------------------------------------------------- joint mapping ----
static void map_pose_to_body(const PoseResult& pr, ParsedBody& body) {
    body.TrackingState = 2;
    body._padding = 0;
    body.TrackingId = 1;

    auto vis_of = [&](int mp) { return pr.vis[mp]; };
    auto st_of = [&](int mp) { return vis_of(mp) > 0.5f ? 2 : 1; };
    auto set = [&](int jt, float x, float y, float z, int st) {
        Joint& j = body.JointData[jt];
        j.TrackingState = st;
        j.PositionX = g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - x) : x;
        j.PositionY = y;
        j.PositionZ = z;
    };
    auto set_mp = [&](int jt, int mp) {
        set(jt, pr.world[mp][0], pr.world[mp][1], pr.world[mp][2], st_of(mp));
    };
    auto set_mid = [&](int jt, int a, int b) {
        set(jt,
            (pr.world[a][0] + pr.world[b][0]) * 0.5f,
            (pr.world[a][1] + pr.world[b][1]) * 0.5f,
            (pr.world[a][2] + pr.world[b][2]) * 0.5f,
            std::min(st_of(a), st_of(b)));
    };
    // core chain
    set_mid(JT_SpineBase, MP_LeftHip, MP_RightHip);                    // 0
    set_mid(JT_Neck, MP_LeftShoulder, MP_RightShoulder);               // 2
    set_mid(JT_SpineMid, MP_LeftHip, MP_RightShoulder);                // placeholder, refined below
    {
        // SpineMid = midpoint(hip-mid, shoulder-mid)
        float hx = (pr.world[MP_LeftHip][0] + pr.world[MP_RightHip][0]) * 0.5f;
        float hy = (pr.world[MP_LeftHip][1] + pr.world[MP_RightHip][1]) * 0.5f;
        float hz = (pr.world[MP_LeftHip][2] + pr.world[MP_RightHip][2]) * 0.5f;
        float sx = (pr.world[MP_LeftShoulder][0] + pr.world[MP_RightShoulder][0]) * 0.5f;
        float sy = (pr.world[MP_LeftShoulder][1] + pr.world[MP_RightShoulder][1]) * 0.5f;
        float sz = (pr.world[MP_LeftShoulder][2] + pr.world[MP_RightShoulder][2]) * 0.5f;
        set(JT_SpineMid, (hx + sx) * 0.5f, (hy + sy) * 0.5f, (hz + sz) * 0.5f,
            std::min(st_of(MP_LeftHip), st_of(MP_RightShoulder)));
    }
    set_mp(JT_Nose, MP_Nose);                                          // 25
    // Kinect has a real head center represented by the midpoint of our
    // synthesized ears. Keep the legacy nose extrapolation for webcam pose.
    if (hardware_skeleton_active()) {
        set_mid(JT_Head, MP_LeftEar, MP_RightEar);
    } else {
        float nx = pr.world[MP_Nose][0], ny = pr.world[MP_Nose][1], nz = pr.world[MP_Nose][2];
        float kx = body.JointData[JT_Neck].PositionX;
        float ky = body.JointData[JT_Neck].PositionY;
        float kz = body.JointData[JT_Neck].PositionZ;
        // undo mirror for the vector math: work in unmirrored space via neck
        float hx = g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - nx) : nx;
        float hz2 = kz;
        (void)hz2;
        float dx = nx - kx, dy = ny - ky, dz = nz - kz;
        set(JT_Head, hx + dx * 0.6f, ny + dy * 0.6f + 0.05f, nz + dz * 0.6f, st_of(MP_Nose));
    }
    // limbs
    set_mp(JT_ShoulderLeft, MP_LeftShoulder);
    set_mp(JT_ElbowLeft, MP_LeftElbow);
    set_mp(JT_WristLeft, MP_LeftWrist);
    set_mp(JT_HandLeft, MP_LeftPinky);
    set_mp(JT_ShoulderRight, MP_RightShoulder);
    set_mp(JT_ElbowRight, MP_RightElbow);
    set_mp(JT_WristRight, MP_RightWrist);
    set_mp(JT_HandRight, MP_RightPinky);
    set_mp(JT_HipLeft, MP_LeftHip);
    set_mp(JT_KneeLeft, MP_LeftKnee);
    set_mp(JT_AnkleLeft, MP_LeftAnkle);
    set_mp(JT_FootLeft, MP_LeftFootIndex);
    set_mp(JT_HipRight, MP_RightHip);
    set_mp(JT_KneeRight, MP_RightKnee);
    set_mp(JT_AnkleRight, MP_RightAnkle);
    set_mp(JT_FootRight, MP_RightFootIndex);
    // SpineShoulder = slightly below neck (toward spine-mid)
    {
        const Joint& neck = body.JointData[JT_Neck];
        const Joint& sm = body.JointData[JT_SpineMid];
        set(JT_SpineShoulder,
            (g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - neck.PositionX) : neck.PositionX) * 0.75f + (g_cfg.mirror_x ? (2.0f * g_cfg.hip_stage_x - sm.PositionX) : sm.PositionX) * 0.25f,
            neck.PositionY * 0.75f + sm.PositionY * 0.25f,
            neck.PositionZ * 0.75f + sm.PositionZ * 0.25f,
            std::min(neck.TrackingState, sm.TrackingState));
    }
    set_mp(JT_HandTipLeft, MP_LeftIndex);
    set_mp(JT_ThumbLeft, MP_LeftThumb);
    set_mp(JT_HandTipRight, MP_RightIndex);
    set_mp(JT_ThumbRight, MP_RightThumb);
    set_mp(JT_EyeLeft, MP_LeftEye);
    set_mp(JT_EyeRight, MP_RightEye);
    set_mp(JT_EarLeft, MP_LeftEar);
    set_mp(JT_EarRight, MP_RightEar);
}

static void make_empty_body(ParsedBody& body, int tracking_state) {
    std::memset(&body, 0, sizeof body);
    body.TrackingState = tracking_state;
    body.TrackingId = 1;
}

// ------------------------------------------------------- preview worker ----
static void preview_loop(OnVideoFrameCallback lcb, void* lctx,
                         OnVideoFrameCallback rcb, void* rctx) {
    log("preview: started (left %p right %p)", (void*)lcb, (void*)rcb);
    std::vector<uint8_t> frame;
    const bool skeleton_only = hardware_skeleton_active() &&
                               !hardware_has_color_stream();
    const int effective_preview_fps = skeleton_only ? 1 : g_cfg.preview_fps;
    const auto interval = std::chrono::microseconds(
        1000000 / std::max(1, effective_preview_fps));
    auto next_tick = std::chrono::steady_clock::now();
    static const uint8_t black_pixel[3] = {0, 0, 0};
    if (skeleton_only) {
        log("preview: VP4U_KINECT_NO_COLOR, publishing shared 1x1 sentinel at 1 fps");
    }
    while (!g_previewWk.stop) {
        int w = skeleton_only ? 1
            : (hardware_skeleton_active() ? src_native_width() : g_cfg.capture_w);
        int h = skeleton_only ? 1
            : (hardware_skeleton_active() ? src_native_height() : g_cfg.capture_h);
        if (w <= 0) w = 640;
        if (h <= 0) h = 480;
        if (!skeleton_only) src_get_frame(frame, w, h);
        const uint8_t* pixels = skeleton_only ? black_pixel
            : (frame.empty() ? nullptr : frame.data());
        uint64_t ts = qpc_now_us();
        if (hardware_skeleton_active()) {
            // A skeleton backend is a single source. Both VP4U "eyes" may share this
            // process-lifetime descriptor; ReleaseImageDesc is intentionally
            // a no-op for reusable slots.
            ImageDesc* shared = make_reusable_image_desc(2, pixels, w, h);
            if (shared && lcb) lcb(lctx, ts, shared);
            if (shared && rcb) rcb(rctx, ts, shared);
        } else {
            if (lcb) {
                ImageDesc* d = make_image_desc(pixels, w, h);
                if (d) lcb(lctx, ts, d);
            }
            if (rcb) {
                ImageDesc* d = make_image_desc(pixels, w, h);
                if (d) rcb(rctx, ts, d);
            }
        }
        next_tick += interval;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick < now) next_tick = now;
        sleep_worker_until(g_previewWk, next_tick);
    }
    log("preview: stopped");
}

// Convert Kinect NUI camera space into the coordinate convention emitted by
// the VP4U-compatible pose path. Keep this at the backend boundary because the
// shared coordinate mapper must not contain Kinect-specific assumptions.
static void transform_kinect_pose_to_stage(PoseResult& pose) {
    for (int i = 0; i < kMpLandmarkCount; ++i) {
        const float camera_x = pose.world[i][0];
        pose.world[i][0] = (g_cfg.kinect_flip_x ? -camera_x : camera_x)
                         + g_cfg.kinect_offset_x;
        pose.world[i][1] += g_cfg.kinect_offset_y;
        pose.world[i][2] += g_cfg.kinect_offset_z;
    }
    if (g_cfg.kinect_face_to_face) {
        // Rotate 180 degrees around a vertical line through the configured
        // horizontal center, while retaining the live hip as the depth pivot.
        // Unlike a hip-local yaw, the fixed X
        // pivot mirrors whole-body translation too: performer-left remains
        // screen-left. The rigid X+Z rotation also swaps the shoulder/hip
        // basis naturally, so no semantic left/right swap is needed.
        const float hip_z = (pose.world[MP_LeftHip][2] +
                             pose.world[MP_RightHip][2]) * 0.5f;
        for (int i = 0; i < kMpLandmarkCount; ++i) {
            pose.world[i][0] = 2.0f * g_cfg.kinect_mirror_center_x
                             - pose.world[i][0];
            pose.world[i][2] = 2.0f * hip_z - pose.world[i][2];
        }
    }
}

// ------------------------------------------------------ analysis worker ----
static void analysis_loop(OnPoseFrameCallback cb, void* ctx) {
    const bool hardware = hardware_skeleton_active();
    log("runtime 5/5: analysis started (%s)", g_kinect ? "kinect NUI skeleton"
        : (g_steamvr ? "SteamVR tracked poses"
                     : (g_poseOk ? "MediaPipe Pose Landmarker"
                                 : "MediaPipe unavailable (empty frames)")));
    std::vector<uint8_t> frame;
    ParsedBody body;
    make_empty_body(body, 0);
    int miss = 999; // frames since last valid detection
    uint64_t frame_no = 0;
    uint64_t last_detected_ms = 0;
    uint64_t hardware_generation = 0;
    bool last_hardware_valid = false;
    bool last_webcam_valid = false;
    auto next_webcam_tick = std::chrono::steady_clock::now();
    static const uint8_t black_pixel[3] = {0, 0, 0};
    while (!g_analysisWk.stop) {
        int w = hardware ? src_native_width() : g_cfg.capture_w;
        int h = hardware ? src_native_height() : g_cfg.capture_h;
        if (w <= 0) w = 640;
        if (h <= 0) h = 480;

        bool detected = false;
        if (hardware) {
            // Hardware poses arrive independently of RGB. Wait for a new
            // source generation rather than resubmitting a stale pose.
            PoseResult pr;
            const bool skeleton_streaming = hardware_wait_for_skeleton(
                &pr, &hardware_generation, 100);
            if (!skeleton_streaming) {
                if (g_analysisWk.stop) break;
                continue;
            }
            if (skeleton_streaming && pr.valid) {
                if (g_kinect) transform_kinect_pose_to_stage(pr);
                map_pose_to_body(pr, body);
                detected = true;
            }

            const bool valid_now = skeleton_streaming && pr.valid;
            if (valid_now != last_hardware_valid) {
                if (valid_now) {
                    const float hip_x = (pr.world[MP_LeftHip][0] +
                                         pr.world[MP_RightHip][0]) * 0.5f;
                    const float hip_y = (pr.world[MP_LeftHip][1] +
                                         pr.world[MP_RightHip][1]) * 0.5f;
                    const float hip_z = (pr.world[MP_LeftHip][2] +
                                         pr.world[MP_RightHip][2]) * 0.5f;
                    log(
                        "analysis: %s body -> TRACKED, "
                        "hip=(%.3f, %.3f, %.3f), confidence=%.2f",
                        hardware_name(), hip_x, hip_y, hip_z, pr.confidence);
                } else if (last_hardware_valid) {
                    log("analysis: %s body -> NOT TRACKED", hardware_name());
                }
                last_hardware_valid = valid_now;
            }
        } else {
            src_get_frame(frame, w, h);
            if (g_poseOk && !frame.empty()) {
                PoseResult pr;
                if (g_pose.infer(frame.data(), w, h, &pr) && pr.valid) {
                    map_pose_to_body(pr, body);
                    detected = true;
                }
            }
            if (detected != last_webcam_valid) {
                log("analysis: MediaPipe body -> %s",
                    detected ? "TRACKED" : "NOT TRACKED");
                last_webcam_valid = detected;
            }
        }
        if (detected) {
            miss = 0;
            last_detected_ms = GetTickCount64();
        } else {
            miss++;
            const uint64_t now_ms = GetTickCount64();
            const bool hardware_grace = hardware && body.TrackingState != 0 &&
                last_detected_ms != 0 &&
                now_ms - last_detected_ms <=
                    (uint64_t)hardware_body_hold_ms();
            if (hardware_grace || (!hardware && miss <= 5 && body.TrackingState != 0)) {
                // Bridge short hardware identity/occlusion gaps with the last stable
                // body, but mark every joint inferred. A real departure still
                // becomes NotTracked after the backend's configured hold time.
                body.TrackingState = 1;
                for (int j = 0; j < kJointCount; ++j) {
                    if (body.JointData[j].TrackingState != 0) {
                        body.JointData[j].TrackingState = 1;
                    }
                }
            } else {
                make_empty_body(body, 0);
            }
        }

        uint64_t ts = qpc_now_us();
        ImageDesc* main_img = nullptr;
        ImageDesc* sub_img = nullptr;
        if (hardware) {
            if (g_kinect && g_cfg.kinect_pose_images &&
                g_kinect->has_color_stream()) {
                src_get_frame(frame, w, h);
                const uint8_t* pixels = frame.empty() ? nullptr : frame.data();
                main_img = make_reusable_image_desc(0, pixels, w, h);
                sub_img = make_reusable_image_desc(1, pixels, w, h);
            } else {
                // A persistent 1x1 descriptor is the minimum callback token
                // that carries the real 3D body without RGB traffic.
                main_img = make_reusable_image_desc(0, black_pixel, 1, 1);
            }
        } else {
            const uint8_t* pixels = frame.empty() ? nullptr : frame.data();
            main_img = make_image_desc(pixels, w, h);
            sub_img = make_image_desc(pixels, w, h);
        }
        if (!main_img || (!hardware && !sub_img)) {
            static int drop_log = 0;
            if (++drop_log % 300 == 1)
                log("analysis: image pool cap reached, delivering frame without images");
        }
        cb(ctx, ts,
           &body, 1,
           nullptr, 0,
           nullptr, 0,
           nullptr, 0,
           main_img, sub_img);
        frame_no++;

        if (!hardware) {
            // Webcam inference has no hardware-frame wait, so retain a precise
            // bounded cadence without the old 4 ms rounding oversleep.
            next_webcam_tick += std::chrono::microseconds(
                1000000 / std::max(1, g_cfg.capture_fps));
            const auto now = std::chrono::steady_clock::now();
            if (next_webcam_tick < now) next_webcam_tick = now;
            sleep_worker_until(g_analysisWk, next_webcam_tick);
        }
    }
    log("analysis: stopped after %llu frames", (unsigned long long)frame_no);
}

// ----------------------------------------------------- table functions ----
static uint64_t t_GetCurrentTimestamp(void) { return qpc_now_us(); }
static intptr_t t_RegisterLoggerCallback(OnLogCallback cb, void* ctx) { return logger_add(cb, ctx); }
static void     t_UnregisterLoggerCallback(intptr_t h) { logger_remove(h); }

static void t_AddRefImageDesc(ImageDesc* p) {
    if (!p) return;
    ImgBlock* blk = img_block_of(p);
    if (blk->persistent) return;
    blk->ref.fetch_add(1, std::memory_order_relaxed);
}
static void t_ReleaseImageDesc(ImageDesc* p) {
    if (!p) return;
    ImgBlock* blk = img_block_of(p);
    if (blk->persistent) return;
    if (blk->ref.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        bool pooled = false;
        {
            std::lock_guard<std::mutex> lk(g_imgPoolMtx);
            if (g_imgPool.size() < kImgPoolKeep) {
                g_imgPool.push_back(blk);
                pooled = true;
            }
        }
        if (!pooled) {
            std::free(blk);
            g_imgAllocated.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
}

static intptr_t t_Initialize(void) {
    config_apply_environment();
    g_initialized = true;
    const char* name = backend() == Backend::Kinect ? "kinect"
        : (backend() == Backend::SteamVr ? "steamvr" : "webcam");
    log("runtime 3/5: initialized VP4U shim (backend=%s)", name);
    return 1;
}

static void t_Shutdown(void) {
    log("Shutdown: stopping workers");
    g_previewWk.join();
    g_analysisWk.join();
    g_kinect.reset();
    g_steamvr.reset();
    g_cam.reset();
    if (g_poseOk) { g_pose.shutdown(); g_poseOk = false; }
    g_initialized = false;
    log("Shutdown: done");
}

static void t_LoadConfig(const char* appConfigPath) { config_load_file(appConfigPath); }
static void t_ReloadConfig(void) {
    if (!g_cfgPath.empty()) config_load_file(g_cfgPath.c_str());
    else log("ReloadConfig: no config loaded yet");
}

static bool t_OpenRealSense(void) {
    if (backend() == Backend::Kinect) {
        log("runtime 4/5: opening Kinect v1 sensor");
        KinectTrackingOptions options;
        options.smoothing = g_cfg.kinect_smoothing;
        options.correction = g_cfg.kinect_correction;
        options.prediction = g_cfg.kinect_prediction;
        options.jitter_radius = g_cfg.kinect_jitter_radius;
        options.max_deviation_radius = g_cfg.kinect_max_deviation_radius;
        options.skeleton_lock_ms = g_cfg.kinect_skeleton_lock_ms;
        options.body_hold_ms = g_cfg.kinect_body_hold_ms;
        options.max_joint_speed_mps = g_cfg.kinect_max_joint_speed_mps;
        options.bone_length_tolerance = g_cfg.kinect_bone_length_tolerance;
        options.use_color = g_cfg.kinect_use_color;
        options.color_fps = g_cfg.preview_fps;
        g_kinect = KinectSource::acquire(options);
        if (g_kinect) {
            log("runtime 4/5: Kinect sensor opened (%s+skeleton)",
                g_kinect->has_color_stream() ? "color" : "no-color");
            return true;
        }
        log("runtime 4/5: Kinect sensor FAILED");
        return false;
    }
    if (backend() == Backend::SteamVr) {
        log("runtime 4/5: opening SteamVR tracked-pose runtime");
        SteamVrTrackingOptions options;
        options.runtime_dir = steamvr_runtime_default();
        options.capture_fps = g_cfg.capture_fps;
        options.body_hold_ms = g_cfg.steamvr_body_hold_ms;
        options.stage_hip_x = g_cfg.steamvr_stage_hip_x;
        options.stage_hip_y = g_cfg.steamvr_stage_hip_y;
        options.stage_hip_z = g_cfg.steamvr_stage_hip_z;
        options.auto_center = g_cfg.steamvr_auto_center;
        options.face_to_face = g_cfg.steamvr_face_to_face;
        options.require_waist = g_cfg.steamvr_require_waist;
        options.require_feet = g_cfg.steamvr_require_feet;
        options.log = [](void*, const char* message) { log("%s", message); };
        std::string error;
        g_steamvr = SteamVrSource::acquire(options, &error);
        if (g_steamvr) {
            log("runtime 4/5: SteamVR runtime opened (tracked poses, no RGB)");
            return true;
        }
        log("runtime 4/5: SteamVR runtime FAILED: %s", error.c_str());
        return false;
    }
    int n = count_physical_cameras();
    log("runtime 4/5: %d USB camera(s), opening index %d",
        n, g_cfg.camera_index);
    if (g_cfg.camera_index < 0 || g_cfg.camera_index >= n) {
        log("runtime 4/5: USB camera FAILED (index out of range)");
        return false;
    }
    g_cam = CameraSource::acquire(g_cfg.camera_index);
    const bool ok = g_cam && g_cam->wait_until_ready(3000);
    if (ok) {
        log("runtime 4/5: USB camera opened (%dx%d)",
            g_cam->native_width(), g_cam->native_height());
    } else {
        log("runtime 4/5: USB camera FAILED (no frame within 3 seconds)");
        g_cam.reset();
    }
    return ok;
}

static bool t_RealSenseLeftIsActive(void) {
    return hardware_skeleton_active() || (g_cam && g_cam->is_real_camera());
}
static bool t_RealSenseRightIsActive(void) {
    return hardware_skeleton_active() || (g_cam && g_cam->is_real_camera());
}

static bool t_TryRebootRealSense(bool forceReboot) {
    log("TryRebootRealSense(force=%d): noop ok", (int)forceReboot);
    return true;
}
static bool t_TryRebuildRealSenseFilters(void) {
    log("TryRebuildRealSenseFilters: noop ok");
    return true;
}

static int init_pose_engine(const char* productKey, const char* mode) {
    (void)productKey;
    if (backend() != Backend::Webcam) {
        // The game initializes VisionPose before it calls OpenRealSense, so
        // the hardware source is still null here. Select by the configured
        // backend to avoid creating an unused MediaPipe thread pool.
        log("InitVisionPose%s: skipped (%s backend supplies tracked poses)",
            mode, backend() == Backend::Kinect ? "kinect" : "steamvr");
        return 1;
    }
    std::wstring runtime_dir;
    std::string model_path;
    paths_default(&runtime_dir, &model_path);
    PoseCalib calib;
    calib.hip_stage_x = g_cfg.hip_stage_x;
    calib.hip_stage_y = g_cfg.hip_stage_y;
    calib.hip_stage_z = g_cfg.hip_stage_z;
    calib.torso_len_m = g_cfg.torso_len_m;
    calib.z_damping = g_cfg.z_damping;
    calib.ema_alpha = g_cfg.ema_alpha;
    std::string err;
    if (g_poseOk) { log("InitVisionPose%s: already initialized", mode); return 1; }
    if (g_pose.init(runtime_dir, model_path, calib, &err)) {
        g_poseOk = true;
        log("InitVisionPose%s: MediaPipe engine ready", mode);
    } else {
        log("InitVisionPose%s: engine FAILED: %s (analysis will emit empty frames)",
            mode, err.c_str());
    }
    // The compatibility API uses 1 to acknowledge initialization. Sensor
    // availability is reported separately by the open and analysis stages.
    return 1;
}

static int t_InitVisionPoseRealtime(const char* productKey) {
    return init_pose_engine(productKey, "Realtime");
}
static int t_InitVisionPoseOffline(const char* productKey) {
    return init_pose_engine(productKey, "Offline");
}

static bool t_StartRealtimeCameraPreview(OnVideoFrameCallback lcb, void* lctx,
                                         OnVideoFrameCallback rcb, void* rctx) {
    g_previewWk.join();
    g_previewWk.stop = false;
    g_previewWk.th = std::thread(preview_loop, lcb, lctx, rcb, rctx);
    return true;
}
static void t_StopRealtimeCameraPreview(void) { g_previewWk.join(); }

static bool t_StartStereoCameraCalibration(OnCalibrationStatusCallback progressCb,
                                           OnVideoFrameCallback previewCb, void* ctx) {
    (void)previewCb;
    log("StartStereoCameraCalibration: skipped (compat layer)");
    if (progressCb) progressCb(ctx, 3, 0, 0, "calibration not required (compat layer)");
    return true;
}
static bool t_CommitStereoCameraCalibration(void) { return true; }
static void t_CloseStereoCameraCalibration(void) {}

static bool t_StartRealtimeAnalysis(OnPoseFrameCallback cb, void* ctx) {
    if (!cb) return false;
    g_analysisWk.join();
    g_analysisWk.stop = false;
    g_analysisWk.th = std::thread(analysis_loop, cb, ctx);
    return true;
}
static void t_StopRealtimeAnalysis(void) { g_analysisWk.join(); }

static bool t_PrepareVideoRecording(const char* path) {
    log("PrepareVideoRecording('%s'): noop ok", path ? path : "");
    return true;
}
static bool t_StartVideoRecording(void) { return true; }
static bool t_FinalizeVideoRecording(void) { return true; }
static bool t_DeleteRecordedVideo(const char* path) { (void)path; return true; }
static bool t_StartOfflineAnalysis(const char* path, OnPoseFrameCallback cb, void* ctx) {
    (void)path; (void)cb; (void)ctx;
    log("StartOfflineAnalysis: noop ok");
    return true;
}
static void t_StopOfflineAnalysis(void) {}

} // namespace vp4u

// ------------------------------------------------------------ exports ----
using namespace vp4u;

extern "C" {

__declspec(dllexport) int vp4uGetVersion() { return kApiVersion; }

__declspec(dllexport) bool vp4uPreboot(const char* prebootConfigJson,
                                       OnLogCallback loggerCb, void* loggerCtx) {
    if (loggerCb) logger_add(loggerCb, loggerCtx);
    log("runtime 1/5: host entered VP4U (config=%s)",
        prebootConfigJson ? prebootConfigJson : "(null)");
    return true;
}

__declspec(dllexport) void vp4uShutdown() {
    log("vp4uShutdown");
    if (g_initialized) t_Shutdown();
}

__declspec(dllexport) bool vp4uGetApiTable(int apiVersion, FunctionPointerTable* table,
                                           OnLogCallback loggerCb, void* loggerCtx) {
    if (loggerCb) logger_add(loggerCb, loggerCtx);
    if (!table || apiVersion != kApiVersion) {
        log("vp4uGetApiTable: rejected (apiVersion=%d table=%p)", apiVersion, (void*)table);
        return false;
    }
    table->GetCurrentTimestamp = t_GetCurrentTimestamp;
    table->RegisterLoggerCallback = t_RegisterLoggerCallback;
    table->UnregisterLoggerCallback = t_UnregisterLoggerCallback;
    table->AddRefImageDesc = t_AddRefImageDesc;
    table->ReleaseImageDesc = t_ReleaseImageDesc;
    table->Initialize = t_Initialize;
    table->Shutdown = t_Shutdown;
    table->LoadConfig = t_LoadConfig;
    table->ReloadConfig = t_ReloadConfig;
    table->OpenRealSense = t_OpenRealSense;
    table->RealSenseLeftIsActive = t_RealSenseLeftIsActive;
    table->RealSenseRightIsActive = t_RealSenseRightIsActive;
    table->TryRebootRealSense = t_TryRebootRealSense;
    table->TryRebuildRealSenseFilters = t_TryRebuildRealSenseFilters;
    table->InitVisionPoseRealtime = t_InitVisionPoseRealtime;
    table->InitVisionPoseOffline = t_InitVisionPoseOffline;
    table->StartRealtimeCameraPreview = t_StartRealtimeCameraPreview;
    table->StopRealtimeCameraPreview = t_StopRealtimeCameraPreview;
    table->StartStereoCameraCalibration = t_StartStereoCameraCalibration;
    table->CommitStereoCameraCalibration = t_CommitStereoCameraCalibration;
    table->CloseStereoCameraCalibration = t_CloseStereoCameraCalibration;
    table->StartRealtimeAnalysis = t_StartRealtimeAnalysis;
    table->StopRealtimeAnalysis = t_StopRealtimeAnalysis;
    table->PrepareVideoRecording = t_PrepareVideoRecording;
    table->StartVideoRecording = t_StartVideoRecording;
    table->FinalizeVideoRecording = t_FinalizeVideoRecording;
    table->DeleteRecordedVideo = t_DeleteRecordedVideo;
    table->StartOfflineAnalysis = t_StartOfflineAnalysis;
    table->StopOfflineAnalysis = t_StopOfflineAnalysis;
    log("runtime 2/5: handed 29-slot VP4U table to host (api=%d)", apiVersion);
    return true;
}

} // extern "C"
