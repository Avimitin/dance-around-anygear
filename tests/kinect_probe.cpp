// kinect_probe.cpp - standalone KinectSource smoke test: open the sensor, then
// once per second report skeleton-stream and body-tracking state.
// Not part of the DLL.
#include "vp4u/kinect.h"

#include <cstdlib>
#include <cstdio>
#include <windows.h>

int main(int argc, char** argv) {
    int run_seconds = argc > 1 ? std::atoi(argv[1]) : 12;
    if (run_seconds < 3) run_seconds = 3;
    if (run_seconds > 120) run_seconds = 120;

    vp4u::KinectTrackingOptions options;
    options.use_color = false;
    auto ks = vp4u::KinectSource::acquire(options);
    if (!ks) {
        std::fprintf(stderr, "KinectSource::acquire FAILED (no Kinect10.dll or no device?)\n");
        return 1;
    }
    std::fprintf(stderr, "kinect open ok, mode=%s\n",
                 ks->has_color_stream() ? "color+skeleton" : "skeleton-only");

    int skeleton_stream_samples = 0;
    int tracked_body_samples = 0;
    uint64_t generation = 0;
    for (int sec = 1; sec <= run_seconds; sec++) {
        Sleep(1000);
        vp4u::PoseResult pr;
        bool have = ks->wait_for_skeleton(&pr, &generation, 250);
        if (have) skeleton_stream_samples++;
        if (have && pr.valid) tracked_body_samples++;
        int tracked = 0;
        for (int i = 0; i < vp4u::kMpLandmarkCount; i++)
            if (pr.vis[i] >= 0.99f) tracked++;
        // HipCenter ~= midpoint of MP hips (23/24)
        float hx = (pr.world[vp4u::MP_LeftHip][0] + pr.world[vp4u::MP_RightHip][0]) * 0.5f;
        float hy = (pr.world[vp4u::MP_LeftHip][1] + pr.world[vp4u::MP_RightHip][1]) * 0.5f;
        float hz = (pr.world[vp4u::MP_LeftHip][2] + pr.world[vp4u::MP_RightHip][2]) * 0.5f;
        std::fprintf(stderr,
            "t=%2ds skeleton=%s valid=%d tracked_joints=%2d hip=(%6.3f,%6.3f,%6.3f)\n",
            sec,
            have ? "streaming" : "no-data", (int)pr.valid, tracked, hx, hy, hz);
    }
    ks.reset();
    std::fprintf(stderr,
        "RESULT skeleton_stream=%d/%d tracked_body=%d/%d\n",
        skeleton_stream_samples, run_seconds,
        tracked_body_samples, run_seconds);
    if (skeleton_stream_samples == 0) {
        std::fprintf(stderr, "FAIL: Kinect skeleton stream is not healthy\n");
        return 2;
    }
    if (tracked_body_samples == 0) {
        std::fprintf(stderr,
            "STREAM PASS, BODY NOT SEEN: stand 1.5-3.5m from Kinect with the full body visible\n");
    } else {
        std::fprintf(stderr, "PASS: Kinect stream and tracked body path are both healthy\n");
    }
    return 0;
}
