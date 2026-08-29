#include "vp4u/steamvr.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>

namespace {

std::wstring widen_acp(const char* text) {
    if (!text || !*text) return {};
    const int count = MultiByteToWideChar(CP_ACP, 0, text, -1,
                                          nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), count);
    result.pop_back();
    return result;
}

void log_line(void*, const char* message) {
    std::printf("[steamvr] %s\n", message ? message : "");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: anygear_steamvr_probe <runtime-dir> <seconds> "
            "[--allow-inferred-body]\n");
        return 2;
    }
    const int seconds = std::clamp(std::atoi(argv[2]), 5, 120);
    const bool allow_inferred =
        argc >= 4 && std::string(argv[3]) == "--allow-inferred-body";

    vp4u::SteamVrTrackingOptions options;
    options.runtime_dir = widen_acp(argv[1]);
    options.require_waist = !allow_inferred;
    options.require_feet = !allow_inferred;
    options.log = log_line;

    std::string error;
    std::shared_ptr<vp4u::SteamVrSource> source =
        vp4u::SteamVrSource::acquire(options, &error);
    if (!source) {
        std::fprintf(stderr, "SteamVR open failed: %s\n", error.c_str());
        return 3;
    }

    const uint64_t deadline = GetTickCount64() +
        static_cast<uint64_t>(seconds) * 1000;
    uint64_t generation = 0;
    uint64_t frames = 0;
    uint64_t valid_frames = 0;
    bool last_valid = false;
    while (GetTickCount64() < deadline) {
        vp4u::PoseResult pose;
        if (!source->wait_for_skeleton(&pose, &generation, 500)) continue;
        ++frames;
        if (pose.valid) ++valid_frames;
        if (pose.valid != last_valid) {
            std::printf("body=%s confidence=%.2f generation=%llu\n",
                pose.valid ? "TRACKED" : "NOT_TRACKED", pose.confidence,
                static_cast<unsigned long long>(generation));
            last_valid = pose.valid;
        }
    }

    std::printf("frames=%llu valid=%llu mode=%s\n",
        static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(valid_frames),
        allow_inferred ? "explicit-inferred" : "strict-six-point");
    if (frames == 0) return 4;
    if (valid_frames == 0) return 5;
    return 0;
}
