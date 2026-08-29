// SPDX-License-Identifier: GPL-3.0-or-later
// Verifies the stock-Spice fallback without launching Unity or the game.

#include "anygear/spice_sdk.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

namespace {

anygear_spice_destroy_callback_func g_destroy = nullptr;
bool g_use_sdk_hook = false;
unsigned int g_sdk_hook_count = 0;

template <typename T>
T load_proc(HMODULE module, const char *name) {
    const FARPROC raw = GetProcAddress(module, name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

ANYGEAR_SPICE_STATUS __cdecl fake_log(
    ANYGEAR_SPICE_LOG_LEVEL level, const char *module, const char *message) {
    std::fprintf(stderr, "[sdk:%d:%s] %s\n", static_cast<int>(level),
                 module ? module : "?", message ? message : "");
    return ANYGEAR_SPICE_SUCCESS;
}

ANYGEAR_SPICE_STATUS __cdecl fake_toast(
    ANYGEAR_SPICE_TOAST_SEVERITY severity, const char *message) {
    std::fprintf(stderr, "[toast:%d] %s\n", static_cast<int>(severity),
                 message ? message : "");
    return ANYGEAR_SPICE_SUCCESS;
}

ANYGEAR_SPICE_STATUS __cdecl fake_hook_library(
    const char *library_name, void *module) {
    if (!library_name || !library_name[0] || !module) {
        return ANYGEAR_SPICE_INVALID_ARGUMENT_1;
    }
    ++g_sdk_hook_count;
    return ANYGEAR_SPICE_SUCCESS;
}

ANYGEAR_SPICE_STATUS __cdecl fake_init(
    uint32_t version,
    anygear_spice_destroy_callback_func destroy,
    void *functions) {
    if (version != 0 || !destroy || !functions) {
        return ANYGEAR_SPICE_INVALID_ARGUMENT_1;
    }
    auto *sdk = static_cast<ANYGEAR_SPICE_SDK_V0 *>(functions);
    const uint32_t size = sdk->size;
    if (size < sizeof(ANYGEAR_SPICE_SDK_V0)) {
        return ANYGEAR_SPICE_TOO_SMALL;
    }
    std::memset(sdk, 0, size);
    sdk->size = size;
    sdk->log = fake_log;
    sdk->add_toast = fake_toast;
    if (g_use_sdk_hook) {
        sdk->hook_library = fake_hook_library;
    }
    // Otherwise hook_library remains null to exercise the stock-Spice IAT
    // fallback.
    g_destroy = destroy;
    return ANYGEAR_SPICE_SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 ||
        (argc == 3 && std::strcmp(argv[2], "--sdk-hook") != 0)) {
        std::fprintf(stderr,
            "usage: loader_smoke.exe <anygear-plugin.dll> [--sdk-hook]\n");
        return 64;
    }
    g_use_sdk_hook = argc == 3;

    HMODULE plugin = LoadLibraryA(argv[1]);
    if (!plugin) {
        std::fprintf(stderr, "plugin LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    using EntryPoint = int (__cdecl *)(anygear_spice_init_func);
    auto entry = load_proc<EntryPoint>(plugin, "spice_sdk_entry_point");
    if (!entry || entry(fake_init) != 1) {
        std::fprintf(stderr, "plugin entry point failed\n");
        return 2;
    }
    if (g_use_sdk_hook && g_sdk_hook_count < 5) {
        std::fprintf(stderr, "Spice SDK library aliases were not registered\n");
        return 3;
    }

    using GetBuildInfo = const char *(__cdecl *)();
    auto get_build_info = load_proc<GetBuildInfo>(
        plugin, "danceAroundAnygearGetBuildInfo");
    if (!get_build_info || !std::strstr(get_build_info(), "vp4u=210") ||
        !std::strstr(get_build_info(), "layout=monolithic")) {
        std::fprintf(stderr, "plugin build information is missing or invalid\n");
        return 4;
    }

    // Stock Spice exercises the plugin's narrow IAT fallback. In SDK mode the
    // fake host has already verified each hook_library registration; load the
    // same plugin once more to hold the reference checked below.
    HMODULE redirected = g_use_sdk_hook
        ? LoadLibraryA(argv[1])
        : LoadLibraryW(
            L"Z:\\dance-around-anygear-does-not-exist\\visionposewrapper.dll");
    if (!redirected) {
        std::fprintf(stderr, "redirected LoadLibrary failed: %lu\n", GetLastError());
        return 5;
    }
    if (redirected != plugin) {
        std::fprintf(stderr, "redirect did not return the monolithic plugin\n");
        return 6;
    }

    using GetVersion = int (*)();
    auto get_version = load_proc<GetVersion>(redirected, "vp4uGetVersion");
    if (!get_version || get_version() != 210) {
        std::fprintf(stderr, "redirected module has the wrong VP4U ABI\n");
        return 7;
    }

    FreeLibrary(redirected);
    if (g_destroy) {
        g_destroy();
    }
    FreeLibrary(plugin);
    std::fprintf(stderr, "PASS: plugin loader redirect\n");
    return 0;
}
