// Exercises the VP4U compatibility surface: GetApiTable(210) -> Initialize ->
// LoadConfig -> OpenRealSense ->
// InitVisionPoseRealtime -> StartRealtimeAnalysis, runs ~15s and reports.
#include "vp4u/vp4u_abi.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include <windows.h>

using namespace vp4u;

static FunctionPointerTable T;
static std::atomic<bool> g_pose_engine_ready{false};

template <typename TProc>
TProc load_proc(HMODULE module, const char *name) {
    const FARPROC raw = GetProcAddress(module, name);
    TProc result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

static void on_log(void*, const char* text) {
    std::fprintf(stderr, "[dll] %s\n", text);
    if (text && std::strstr(text, "MediaPipe engine ready")) {
        g_pose_engine_ready = true;
    }
}

struct Stats {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> tracked{0};   // TrackingState==2
    std::atomic<uint64_t> inferred{0};  // TrackingState==1
    std::atomic<uint64_t> empty{0};     // TrackingState==0
    std::atomic<uint64_t> images{0};
    std::atomic<uint64_t> joint_checksum_errors{0};
    std::atomic<uint64_t> degenerate_pose_errors{0};
    std::atomic<uint64_t> limb_geometry_errors{0};
    std::atomic<uint64_t> endpoint_jump_errors{0};
    std::atomic<uint64_t> inferred_joint_samples{0};
    double max_endpoint_step = 0;
    bool have_previous_body = false;
    Joint previous_joint[kJointCount] {};
    double joint_sink = 0; // prevents optimizing out the joint reads
};

static Stats g_stats;

static void on_pose_frame(void*, uint64_t timestamp,
                          ParsedBody* pose3d, int pose3dCount,
                          ParsedBody*, int,
                          ParsedBody*, int,
                          ParsedBody*, int,
                          ImageDesc* mainImg, ImageDesc* subImg) {
    g_stats.frames++;
    for (int i = 0; i < pose3dCount; i++) {
        ParsedBody& b = pose3d[i];
        // read ALL 30 joints fully (memory-safety check: must not fault)
        double acc = 0;
        for (int j = 0; j < kJointCount; j++) {
            const Joint& joint = b.JointData[j];
            if (joint.TrackingState < 0 || joint.TrackingState > 2 ||
                !std::isfinite(joint.PositionX) ||
                !std::isfinite(joint.PositionY) ||
                !std::isfinite(joint.PositionZ)) {
                g_stats.joint_checksum_errors++;
            }
            if (joint.TrackingState == 1) g_stats.inferred_joint_samples++;
            acc += b.JointData[j].PositionX + b.JointData[j].PositionY
                 + b.JointData[j].PositionZ + b.JointData[j].TrackingState;
        }
        g_stats.joint_sink += acc;
        if (b.TrackingState == 2) g_stats.tracked++;
        else if (b.TrackingState == 1) g_stats.inferred++;
        else g_stats.empty++;

        if (b.TrackingState == 2) {
            auto distance = [&](int a, int c) {
                const Joint& p = b.JointData[a];
                const Joint& q = b.JointData[c];
                const double dx = p.PositionX - q.PositionX;
                const double dy = p.PositionY - q.PositionY;
                const double dz = p.PositionZ - q.PositionZ;
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            };
            const Joint& earLeft = b.JointData[JT_EarLeft];
            const Joint& earRight = b.JointData[JT_EarRight];
            const Joint& nose = b.JointData[JT_Nose];
            const double earMidX = (earLeft.PositionX + earRight.PositionX) * 0.5;
            const double earMidY = (earLeft.PositionY + earRight.PositionY) * 0.5;
            const double earMidZ = (earLeft.PositionZ + earRight.PositionZ) * 0.5;
            const double noseDx = nose.PositionX - earMidX;
            const double noseDy = nose.PositionY - earMidY;
            const double noseDz = nose.PositionZ - earMidZ;
            const double noseDepth = std::sqrt(
                noseDx * noseDx + noseDy * noseDy + noseDz * noseDz);
            if (distance(JT_EarLeft, JT_EarRight) < 0.05 || noseDepth < 0.03 ||
                distance(JT_WristLeft, JT_HandLeft) < 0.01 ||
                distance(JT_HandLeft, JT_HandTipLeft) < 0.01 ||
                distance(JT_HandLeft, JT_ThumbLeft) < 0.005 ||
                distance(JT_WristRight, JT_HandRight) < 0.01 ||
                distance(JT_HandRight, JT_HandTipRight) < 0.01 ||
                distance(JT_HandRight, JT_ThumbRight) < 0.005) {
                g_stats.degenerate_pose_errors++;
            }
            if (distance(JT_ShoulderLeft, JT_ElbowLeft) > 0.75 ||
                distance(JT_ElbowLeft, JT_WristLeft) > 0.75 ||
                distance(JT_ShoulderRight, JT_ElbowRight) > 0.75 ||
                distance(JT_ElbowRight, JT_WristRight) > 0.75 ||
                distance(JT_HipLeft, JT_KneeLeft) > 0.90 ||
                distance(JT_KneeLeft, JT_AnkleLeft) > 0.90 ||
                distance(JT_HipRight, JT_KneeRight) > 0.90 ||
                distance(JT_KneeRight, JT_AnkleRight) > 0.90 ||
                distance(JT_AnkleLeft, JT_FootLeft) > 0.50 ||
                distance(JT_AnkleRight, JT_FootRight) > 0.50) {
                g_stats.limb_geometry_errors++;
            }
        }

        if (b.TrackingState != 0) {
            if (g_stats.have_previous_body) {
                const int endpoints[] = {
                    JT_WristLeft, JT_HandLeft, JT_HandTipLeft,
                    JT_WristRight, JT_HandRight, JT_HandTipRight,
                    JT_AnkleLeft, JT_FootLeft,
                    JT_AnkleRight, JT_FootRight
                };
                for (int j : endpoints) {
                    const Joint& current = b.JointData[j];
                    const Joint& previous = g_stats.previous_joint[j];
                    const double dx = current.PositionX - previous.PositionX;
                    const double dy = current.PositionY - previous.PositionY;
                    const double dz = current.PositionZ - previous.PositionZ;
                    const double step = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (step > g_stats.max_endpoint_step) {
                        g_stats.max_endpoint_step = step;
                    }
                    // At 30 Hz this is >10.5 m/s. It is an endpoint explosion,
                    // not a possible dance movement.
                    if (step > 0.35) g_stats.endpoint_jump_errors++;
                }
            }
            std::memcpy(g_stats.previous_joint, b.JointData,
                        sizeof g_stats.previous_joint);
            g_stats.have_previous_body = true;
        } else {
            g_stats.have_previous_body = false;
        }

        uint64_t f = g_stats.frames;
        if (f % 60 == 1) {
            const Joint& head = b.JointData[JT_Head];
            const Joint& fl = b.JointData[JT_FootLeft];
            const Joint& fr = b.JointData[JT_FootRight];
            std::fprintf(stderr,
                "[frame %4llu] ts=%llu body(ts=%d id=%lld) "
                "Head=(%.2f,%.2f,%.2f) FootL=(%.2f,%.2f,%.2f) FootR=(%.2f,%.2f,%.2f)\n",
                (unsigned long long)f, (unsigned long long)timestamp,
                b.TrackingState, (long long)b.TrackingId,
                head.PositionX, head.PositionY, head.PositionZ,
                fl.PositionX, fl.PositionY, fl.PositionZ,
                fr.PositionX, fr.PositionY, fr.PositionZ);
        }
    }
    // Callback images are released when the consumer is done with them.
    if (mainImg) {
        if (mainImg->PointerToFrame && mainImg->BytesPerFrame > 0) g_stats.images++;
        T.ReleaseImageDesc(mainImg);
    }
    if (subImg) {
        if (subImg->PointerToFrame && subImg->BytesPerFrame > 0) g_stats.images++;
        T.ReleaseImageDesc(subImg);
    }
}

static std::atomic<uint64_t> g_previewFrames{0};
static void on_preview(void*, uint64_t, ImageDesc* img) {
    g_previewFrames++;
    if (img) {
        if (img->BitsPerPixel != 24 || img->BytesPerRow != img->Width * 3)
            g_stats.joint_checksum_errors++;
        T.ReleaseImageDesc(img);
    }
}

int main(int argc, char** argv) {
    const char* dll_path = argc > 1
        ? argv[1]
        : ".\\dance_around_anygear_kinect.dll";
    const char* cfg_path = argc > 2 ? argv[2] : "vp4u-config.json";
    int run_secs = argc > 3 ? atoi(argv[3]) : 15;

    HMODULE dll = LoadLibraryA(dll_path);
    if (!dll) { std::fprintf(stderr, "LoadLibrary('%s') failed: %lu\n", dll_path, GetLastError()); return 1; }

    auto pGetVersion = load_proc<decltype(vp4uGetVersion)*>(dll, "vp4uGetVersion");
    auto pPreboot = load_proc<decltype(vp4uPreboot)*>(dll, "vp4uPreboot");
    auto pShutdown = load_proc<decltype(vp4uShutdown)*>(dll, "vp4uShutdown");
    auto pGetApiTable = load_proc<decltype(vp4uGetApiTable)*>(dll, "vp4uGetApiTable");
    if (!pGetVersion || !pPreboot || !pShutdown || !pGetApiTable) {
        std::fprintf(stderr, "missing exports (ver=%p preboot=%p shutdown=%p table=%p)\n",
                     (void*)pGetVersion, (void*)pPreboot, (void*)pShutdown, (void*)pGetApiTable);
        return 1;
    }

    int ver = pGetVersion();
    std::fprintf(stderr, "vp4uGetVersion() = %d (expect 210)\n", ver);
    if (ver != 210) return 1;

    pPreboot(nullptr, nullptr, nullptr);

    std::memset(&T, 0, sizeof T);
    if (!pGetApiTable(210, &T, on_log, nullptr)) {
        std::fprintf(stderr, "GetApiTable(210) rejected\n");
        return 1;
    }
    // verify all 29 slots are non-null
    void** slots = (void**)&T;
    int nulls = 0;
    for (int i = 0; i < 29; i++) if (!slots[i]) { std::fprintf(stderr, "slot %d is NULL\n", i); nulls++; }
    if (nulls) return 1;
    std::fprintf(stderr, "api table: 29 slots filled ok\n");

    if (argc > 2 && std::strcmp(argv[2], "--abi-only") == 0) {
        pShutdown();
        FreeLibrary(dll);
        std::fprintf(stderr, "PASS: ABI only\n");
        return 0;
    }

    intptr_t init_h = T.Initialize();
    std::fprintf(stderr, "Initialize() -> %lld\n", (long long)init_h);

    const bool pose_init_only = argc > 2 &&
        std::strcmp(argv[2], "--pose-init-only") == 0;
    const bool pose_init_config = argc > 2 &&
        std::strcmp(argv[2], "--pose-init-config") == 0;
    if (pose_init_only || pose_init_config) {
        if (pose_init_config && argc < 4) {
            std::fprintf(stderr,
                "--pose-init-config requires a VP4U JSON path\n");
            return 2;
        }
        T.LoadConfig(pose_init_config ? argv[3] : nullptr);
        const int result = T.InitVisionPoseRealtime("dummy-key");
        T.Shutdown();
        pShutdown();
        FreeLibrary(dll);
        const bool pass = result == 1 && g_pose_engine_ready;
        std::fprintf(stderr, "%s: pose initialization %s\n",
                     pass ? "PASS" : "FAIL", pass ? "ready" : "FAILED");
        return pass ? 0 : 1;
    }

    T.LoadConfig(cfg_path);

    bool open_ok = T.OpenRealSense();
    std::fprintf(stderr, "OpenRealSense() -> %d, L=%d R=%d\n", (int)open_ok,
                 (int)T.RealSenseLeftIsActive(), (int)T.RealSenseRightIsActive());

    int rc = T.InitVisionPoseRealtime("dummy-key");
    std::fprintf(stderr, "InitVisionPoseRealtime() -> %d\n", rc);

    // camera preview (left+right) like the game does during attract
    T.StartRealtimeCameraPreview(on_preview, nullptr, on_preview, nullptr);

    if (!T.StartRealtimeAnalysis(on_pose_frame, nullptr)) {
        std::fprintf(stderr, "StartRealtimeAnalysis failed\n");
        return 1;
    }

    std::fprintf(stderr, "running %d seconds...\n", run_secs);
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(run_secs))
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    T.StopRealtimeAnalysis();
    T.StopRealtimeCameraPreview();

    // exercise the stub functions
    bool stubs_ok = true;
    stubs_ok &= T.TryRebootRealSense(false);
    stubs_ok &= T.TryRebuildRealSenseFilters();
    stubs_ok &= T.PrepareVideoRecording("mnt/captures/test.mp4");
    stubs_ok &= T.StartVideoRecording();
    stubs_ok &= T.FinalizeVideoRecording();
    stubs_ok &= T.DeleteRecordedVideo("mnt/captures/test.mp4");
    stubs_ok &= T.StartOfflineAnalysis("mnt/captures/test.mp4", on_pose_frame, nullptr);
    T.StopOfflineAnalysis();
    stubs_ok &= T.StartStereoCameraCalibration([](void*, int status, int, int, const char* msg) {
        std::fprintf(stderr, "[calib] status=%d msg=%s\n", status, msg ? msg : "");
    }, nullptr, nullptr);
    stubs_ok &= T.CommitStereoCameraCalibration();
    T.CloseStereoCameraCalibration();
    std::fprintf(stderr, "stubs: %s\n", stubs_ok ? "all ok" : "SOME FAILED");

    // timestamp monotonicity
    uint64_t ts1 = T.GetCurrentTimestamp();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t ts2 = T.GetCurrentTimestamp();
    std::fprintf(stderr, "GetCurrentTimestamp: %llu -> %llu (%s, dt=%llu us)\n",
                 (unsigned long long)ts1, (unsigned long long)ts2,
                 ts2 > ts1 ? "monotonic" : "BROKEN", (unsigned long long)(ts2 - ts1));

    T.Shutdown();
    pShutdown();

    uint64_t frames = g_stats.frames;
    std::fprintf(stderr, "\n==== RESULTS ====\n");
    std::fprintf(stderr, "pose frames: %llu in %.1fs = %.1f fps (need >=28)\n",
                 (unsigned long long)frames, elapsed, frames / elapsed);
    std::fprintf(stderr, "  tracked(2)=%llu inferred(1)=%llu empty(0)=%llu\n",
                 (unsigned long long)g_stats.tracked, (unsigned long long)g_stats.inferred,
                 (unsigned long long)g_stats.empty);
    std::fprintf(stderr, "  valid images received+released: %llu\n", (unsigned long long)g_stats.images);
    std::fprintf(stderr, "  preview frames: %llu in %.1fs = %.1f fps\n",
                 (unsigned long long)g_previewFrames, elapsed, g_previewFrames / elapsed);
    std::fprintf(stderr, "  struct/joint errors: %llu\n", (unsigned long long)g_stats.joint_checksum_errors);
    std::fprintf(stderr, "  degenerate tracked poses: %llu\n",
                 (unsigned long long)g_stats.degenerate_pose_errors);
    std::fprintf(stderr, "  implausible limb geometries: %llu\n",
                 (unsigned long long)g_stats.limb_geometry_errors);
    std::fprintf(stderr, "  endpoint jumps >0.35m/frame: %llu (max %.3fm)\n",
                 (unsigned long long)g_stats.endpoint_jump_errors,
                 g_stats.max_endpoint_step);
    std::fprintf(stderr, "  inferred joint samples: %llu\n",
                 (unsigned long long)g_stats.inferred_joint_samples);
    std::fprintf(stderr, "  joint sink (ignore): %.3f\n", g_stats.joint_sink);

    // NUI is nominally 30 Hz; allow scheduler/measurement boundary jitter but
    // reject the old image-contention path, which fell to roughly 7 Hz.
    bool pass = frames > 0 && (frames / elapsed) >= 28.0 &&
                g_stats.joint_checksum_errors == 0 &&
                g_stats.degenerate_pose_errors == 0 &&
                g_stats.limb_geometry_errors == 0 &&
                g_stats.endpoint_jump_errors == 0;
    std::fprintf(stderr, "PASS: %s\n", pass ? "yes" : "NO");
    return pass ? 0 : 2;
}
