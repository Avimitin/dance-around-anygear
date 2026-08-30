// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises the native D4xx IAT adapter without game files or camera hardware.

#include "anygear/spice_sdk.h"
#include "vp4u/vp4u_abi.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <windows.h>

#include <librealsense2/rs.h>

namespace {

HMODULE g_original_wrapper = nullptr;
HMODULE g_alias_module = nullptr;
unsigned int g_alias_count = 0;
anygear_spice_destroy_callback_func g_destroy = nullptr;
bool g_offer_library_alias = true;

template <typename T>
T load_proc(HMODULE module, const char* name) {
    const FARPROC raw = GetProcAddress(module, name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

ANYGEAR_SPICE_STATUS __cdecl fake_log(
    ANYGEAR_SPICE_LOG_LEVEL level, const char* module, const char* message) {
    std::fprintf(stderr, "[sdk:%d:%s] %s\n", static_cast<int>(level),
                 module ? module : "?", message ? message : "");
    return ANYGEAR_SPICE_SUCCESS;
}

ANYGEAR_SPICE_STATUS __cdecl fake_toast(
    ANYGEAR_SPICE_TOAST_SEVERITY, const char* message) {
    std::fprintf(stderr, "[toast] %s\n", message ? message : "");
    return ANYGEAR_SPICE_SUCCESS;
}

ANYGEAR_SPICE_STATUS __cdecl fake_hook_library(
    const char* name, void* module) {
    if (!name || !name[0] || !module) return ANYGEAR_SPICE_INVALID_ARGUMENT_1;
    HMODULE candidate = static_cast<HMODULE>(module);
    if (g_alias_module && g_alias_module != candidate) {
        return ANYGEAR_SPICE_GENERIC_ERROR;
    }
    g_alias_module = candidate;
    ++g_alias_count;
    return ANYGEAR_SPICE_SUCCESS;
}

ANYGEAR_SPICE_STATUS __cdecl fake_init(
    std::uint32_t version, anygear_spice_destroy_callback_func destroy,
    void* functions) {
    if (version != 0 || !destroy || !functions) {
        return ANYGEAR_SPICE_INVALID_ARGUMENT_1;
    }
    auto* sdk = static_cast<ANYGEAR_SPICE_SDK_V0*>(functions);
    const std::uint32_t size = sdk->size;
    std::memset(sdk, 0, size);
    sdk->size = size;
    sdk->log = fake_log;
    sdk->add_toast = fake_toast;
    if (g_offer_library_alias) sdk->hook_library = fake_hook_library;
    g_destroy = destroy;
    return ANYGEAR_SPICE_SUCCESS;
}

bool expect(bool value, const char* message) {
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

} // namespace

int main(int argc, char** argv) {
    bool load_only = false;
    bool legacy_host = false;
    bool valid_arguments = argc >= 3 && argc <= 5;
    for (int index = 3; valid_arguments && index < argc; ++index) {
        if (std::strcmp(argv[index], "--load-only") == 0 && !load_only) {
            load_only = true;
        } else if (std::strcmp(argv[index], "--legacy-host") == 0 &&
                   !legacy_host) {
            legacy_host = true;
        } else {
            valid_arguments = false;
        }
    }
    if (!valid_arguments) {
        std::fprintf(stderr,
            "usage: d4xx_native_bridge_smoke <bridge.dll> "
            "<visionposewrapper.dll> [--load-only] [--legacy-host]\n");
        return 64;
    }
    g_offer_library_alias = !legacy_host;
    std::filesystem::path wrapper =
        std::filesystem::absolute(std::filesystem::path(argv[2]));
    wrapper.make_preferred();
    HMODULE bridge = LoadLibraryA(argv[1]);
    if (!expect(bridge != nullptr, "native bridge did not load")) return 1;
    g_original_wrapper = LoadLibraryExW(
        wrapper.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_original_wrapper) {
        std::fprintf(stderr, "LoadLibraryExW failed with Win32 %lu\n",
                     static_cast<unsigned long>(GetLastError()));
    }
    if (!expect(g_original_wrapper != nullptr,
                "original wrapper did not load")) {
        return 1;
    }
    using Entry = int (__cdecl*)(anygear_spice_init_func);
    const auto entry = load_proc<Entry>(bridge, "spice_sdk_entry_point");
    if (!expect(entry && entry(fake_init) == 1,
                "native bridge entry point failed")) {
        return 2;
    }
    if (legacy_host) {
        HMODULE late_loader = nullptr;
        if (!load_only) {
            const std::filesystem::path loader_path =
                wrapper.parent_path() / L"UnityPlayer.dll";
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            using SystemLoadLibraryExW =
                HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
            const auto system_load = load_proc<SystemLoadLibraryExW>(
                kernel, "LoadLibraryExW");
            late_loader = system_load
                ? system_load(loader_path.c_str(), nullptr,
                              LOAD_WITH_ALTERED_SEARCH_PATH)
                : nullptr;
            using LoadVp4u = HMODULE (__cdecl*)();
            const auto load_vp4u = load_proc<LoadVp4u>(
                late_loader, "anygearFakeLoadVp4u");
            g_alias_module = load_vp4u ? load_vp4u() : nullptr;
        } else {
            g_alias_module = LoadLibraryW(L"visionposewrapper.dll");
        }
        if (g_alias_module != bridge) {
            std::array<char, 1024> alias_path{};
            GetModuleFileNameA(
                g_alias_module, alias_path.data(),
                static_cast<DWORD>(alias_path.size()));
            std::fprintf(stderr,
                "legacy redirect: bridge=%p result=%p path=%s\n",
                static_cast<void*>(bridge),
                static_cast<void*>(g_alias_module), alias_path.data());
        }
        if (!expect(g_alias_module == bridge && g_alias_count == 0,
                    "loader compatibility did not redirect VP4U")) {
            return 2;
        }
        if (late_loader) {
            using ResolveVp4u = FARPROC (__cdecl*)(HMODULE);
            const auto resolve_vp4u = load_proc<ResolveVp4u>(
                late_loader, "anygearFakeResolveVp4u");
            const FARPROC cached_export = resolve_vp4u
                ? resolve_vp4u(g_original_wrapper)
                : nullptr;
            const FARPROC bridge_export = GetProcAddress(
                bridge, "vp4uGetApiTable");
            if (!expect(cached_export && cached_export == bridge_export,
                        "cached VP4U handle did not resolve through bridge")) {
                return 2;
            }
        }
        if (late_loader) FreeLibrary(late_loader);
    } else if (!expect(
                   g_original_wrapper != nullptr &&
                       g_alias_module == bridge && g_alias_count == 5,
                   "native bridge did not register all VP4U aliases")) {
        return 2;
    }

    using GetApiTable = bool (__cdecl*)(
        int, vp4u::FunctionPointerTable*, vp4u::OnLogCallback, void*);
    const auto get_api_table = load_proc<GetApiTable>(
        g_alias_module, "vp4uGetApiTable");
    vp4u::FunctionPointerTable table{};
    if (!expect(get_api_table && get_api_table(
                    vp4u::kApiVersion, &table, nullptr, nullptr),
                "forwarded VP4U API table failed") ||
        !expect(table.InitVisionPoseRealtime != nullptr,
                "realtime VisionPose entry is absent") ||
        !expect(table.InitVisionPoseOffline &&
                    table.InitVisionPoseOffline("test") == 1,
                "offline initialization was not adapted") ||
        !expect(table.StartOfflineAnalysis &&
                    table.StartOfflineAnalysis(
                        "test.mp4", nullptr, nullptr),
                "offline start was not adapted") ||
        !expect(table.StopOfflineAnalysis != nullptr,
                "offline stop was not adapted")) {
        return 6;
    }
    table.StopOfflineAnalysis();
    if (!load_only &&
        !expect(table.InitVisionPoseRealtime("test") == 7,
                "realtime VisionPose entry was not preserved")) {
        return 6;
    }
    using GetInt = int (__cdecl*)();
    const auto color_sensor_compatible = load_proc<GetInt>(
        g_original_wrapper, "anygearFakeColorSensorCompatible");
    if (!load_only &&
        !expect(color_sensor_compatible && color_sensor_compatible() == 1,
                "depth sensor did not supply virtual color sensor role")) {
        return 6;
    }

    using BuildInfo = const char* (__cdecl*)();
    const auto build_info = load_proc<BuildInfo>(
        bridge, "danceAroundAnygearGetBuildInfo");
    if (!expect(build_info &&
                    std::strstr(build_info(), "mode=native-d4xx-bridge") &&
                    std::strstr(build_info(), "vp4u=original"),
                "native bridge build information is wrong")) {
        return 5;
    }
    if (load_only) {
        if (g_destroy) g_destroy();
        std::fprintf(stderr,
            "PASS: original VP4U forwarded and native D4xx imports adapted\n");
        return 0;
    }

    using Exercise = int (__cdecl*)(std::uint8_t*, int, int*, int);
    const auto exercise = load_proc<Exercise>(
        g_original_wrapper, "anygearFakeExerciseBridge");
    std::array<std::uint8_t, 12> output{};
    std::array<int, 8> metadata{};
    const int size = exercise
        ? exercise(output.data(), static_cast<int>(output.size()),
                   metadata.data(), static_cast<int>(metadata.size()))
        : 0;
    const std::array<std::uint8_t, 12> expected{{
        0, 0, 0, 64, 64, 64,
        128, 128, 128, 255, 255, 255,
    }};
    if (!expect(size == 12, "virtual frame did not become BGR24") ||
        !expect(output == expected, "virtual BGR24 pixels are wrong") ||
        !expect(metadata[0] == RS2_STREAM_COLOR &&
                    metadata[1] == RS2_FORMAT_BGR8 && metadata[2] == 0,
                "wrapper did not observe a COLOR/BGR8 profile") ||
        !expect(metadata[3] == 2 && metadata[4] == 2 &&
                    metadata[5] == 12 && metadata[6] == 6 &&
                    metadata[7] == 24,
                "wrapper did not observe BGR24 frame geometry")) {
        return 3;
    }

    HMODULE runtime = GetModuleHandleW(L"realsense2.dll");
    const auto config_stream =
        load_proc<GetInt>(runtime, "fakeRsLastConfigStream");
    const auto config_format =
        load_proc<GetInt>(runtime, "fakeRsLastConfigFormat");
    const auto config_index =
        load_proc<GetInt>(runtime, "fakeRsLastConfigIndex");
    const auto align_stream =
        load_proc<GetInt>(runtime, "fakeRsLastAlignStream");
    const auto release_count =
        load_proc<GetInt>(runtime, "fakeRsReleaseCount");
    const auto json_has_color =
        load_proc<GetInt>(runtime, "fakeRsJsonContainsColorControls");
    const auto json_has_depth =
        load_proc<GetInt>(runtime, "fakeRsJsonContainsDepthControls");
    if (!expect(runtime && config_stream && config_format && config_index &&
                    align_stream && release_count && json_has_color &&
                    json_has_depth,
                "fake librealsense diagnostics are missing") ||
        !expect(config_stream() == RS2_STREAM_INFRARED &&
                    config_format() == RS2_FORMAT_Y8 && config_index() == 1,
                "physical stream request was not remapped to IR/Y8") ||
        !expect(align_stream() == RS2_STREAM_INFRARED,
                "depth alignment target was not remapped to infrared") ||
        !expect(release_count() == 1,
                "frame release was not forwarded") ||
        !expect(json_has_color() == 0 && json_has_depth() == 1,
                "RGB-only camera JSON controls were not filtered")) {
        return 4;
    }

    if (g_destroy) g_destroy();
    FreeLibrary(g_original_wrapper);
    if (legacy_host) FreeLibrary(g_alias_module);
    FreeLibrary(bridge);
    std::fprintf(stderr,
        "PASS: native D4xx bridge forwards original VP4U and adapts IR\n");
    return 0;
}
