// SPDX-License-Identifier: GPL-3.0-or-later
// Spice -k bootstrap: select a hardware backend and redirect VP4U at runtime.

#include "anygear/spice_sdk.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#ifndef ANYGEAR_VERSION
#define ANYGEAR_VERSION "0.0.0-dev"
#endif

#if defined(ANYGEAR_BACKEND_KINECT)
static constexpr char kBackendName[] = "kinect";
#elif defined(ANYGEAR_BACKEND_WEBCAM)
static constexpr char kBackendName[] = "webcam";
#else
#error Define exactly one ANYGEAR_BACKEND_* target.
#endif

namespace {

using LoadLibraryAFn = HMODULE (WINAPI *)(LPCSTR);
using LoadLibraryWFn = HMODULE (WINAPI *)(LPCWSTR);
using LoadLibraryExAFn = HMODULE (WINAPI *)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE (WINAPI *)(LPCWSTR, HANDLE, DWORD);
using GetModuleHandleAFn = HMODULE (WINAPI *)(LPCSTR);
using GetModuleHandleWFn = HMODULE (WINAPI *)(LPCWSTR);

HMODULE g_self = nullptr;
HMODULE g_payload = nullptr;
ANYGEAR_SPICE_SDK_V0 g_spice{};

LoadLibraryAFn g_load_library_a = nullptr;
LoadLibraryWFn g_load_library_w = nullptr;
LoadLibraryExAFn g_load_library_ex_a = nullptr;
LoadLibraryExWFn g_load_library_ex_w = nullptr;
GetModuleHandleAFn g_get_module_handle_a = nullptr;
GetModuleHandleWFn g_get_module_handle_w = nullptr;

thread_local bool g_inside_loader_hook = false;

void log_message(ANYGEAR_SPICE_LOG_LEVEL level, const std::string &message) {
    if (g_spice.log) {
        g_spice.log(level, "dance_around_anygear", message.c_str());
        return;
    }
    const std::string line = "[dance-around-anygear] " + message + "\n";
    OutputDebugStringA(line.c_str());
}

void toast_error(const std::string &message) {
    log_message(ANYGEAR_SPICE_LOG_WARNING, message);
    if (g_spice.add_toast) {
        g_spice.add_toast(ANYGEAR_SPICE_TOAST_ERROR, message.c_str());
    }
}

std::wstring module_path(HMODULE module) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring parent_path(const std::wstring &path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

const char *base_name(const char *path) {
    if (!path) {
        return nullptr;
    }
    const char *result = path;
    for (const char *cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            result = cursor + 1;
        }
    }
    return result;
}

const wchar_t *base_name(const wchar_t *path) {
    if (!path) {
        return nullptr;
    }
    const wchar_t *result = path;
    for (const wchar_t *cursor = path; *cursor; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            result = cursor + 1;
        }
    }
    return result;
}

bool is_vp4u_name(const char *path) {
    const char *name = base_name(path);
    return name && (_stricmp(name, "visionposewrapper.dll") == 0 ||
                    _stricmp(name, "visionposewrapper") == 0);
}

bool is_vp4u_name(const wchar_t *path) {
    const wchar_t *name = base_name(path);
    return name && (_wcsicmp(name, L"visionposewrapper.dll") == 0 ||
                    _wcsicmp(name, L"visionposewrapper") == 0);
}

HMODULE payload_reference() {
    if (!g_payload) {
        return nullptr;
    }

    HMODULE referenced = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(g_payload),
            &referenced)) {
        return referenced;
    }
    return g_payload;
}

bool patch_module_imports(HMODULE module);

struct LoaderScope {
    bool outer = false;
    LoaderScope() {
        outer = !g_inside_loader_hook;
        g_inside_loader_hook = true;
    }
    ~LoaderScope() {
        if (outer) {
            g_inside_loader_hook = false;
        }
    }
};

HMODULE WINAPI load_library_a_hook(LPCSTR file_name) {
    if (is_vp4u_name(file_name)) {
        return payload_reference();
    }
    if (!g_load_library_a) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_a(file_name);
    if (scope.outer && module) {
        patch_module_imports(module);
    }
    return module;
}

HMODULE WINAPI load_library_w_hook(LPCWSTR file_name) {
    if (is_vp4u_name(file_name)) {
        return payload_reference();
    }
    if (!g_load_library_w) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_w(file_name);
    if (scope.outer && module) {
        patch_module_imports(module);
    }
    return module;
}

HMODULE WINAPI load_library_ex_a_hook(LPCSTR file_name, HANDLE file, DWORD flags) {
    if (is_vp4u_name(file_name)) {
        return payload_reference();
    }
    if (!g_load_library_ex_a) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_ex_a(file_name, file, flags);
    if (scope.outer && module) {
        patch_module_imports(module);
    }
    return module;
}

HMODULE WINAPI load_library_ex_w_hook(LPCWSTR file_name, HANDLE file, DWORD flags) {
    if (is_vp4u_name(file_name)) {
        return payload_reference();
    }
    if (!g_load_library_ex_w) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_ex_w(file_name, file, flags);
    if (scope.outer && module) {
        patch_module_imports(module);
    }
    return module;
}

HMODULE WINAPI get_module_handle_a_hook(LPCSTR module_name) {
    if (is_vp4u_name(module_name)) {
        return g_payload;
    }
    return g_get_module_handle_a ? g_get_module_handle_a(module_name) : nullptr;
}

HMODULE WINAPI get_module_handle_w_hook(LPCWSTR module_name) {
    if (is_vp4u_name(module_name)) {
        return g_payload;
    }
    return g_get_module_handle_w ? g_get_module_handle_w(module_name) : nullptr;
}

struct ImportReplacement {
    const char *name;
    void *replacement;
    void **original;
};

std::array<ImportReplacement, 6> import_replacements() {
    return {{
        {"LoadLibraryA", reinterpret_cast<void *>(load_library_a_hook),
            reinterpret_cast<void **>(&g_load_library_a)},
        {"LoadLibraryW", reinterpret_cast<void *>(load_library_w_hook),
            reinterpret_cast<void **>(&g_load_library_w)},
        {"LoadLibraryExA", reinterpret_cast<void *>(load_library_ex_a_hook),
            reinterpret_cast<void **>(&g_load_library_ex_a)},
        {"LoadLibraryExW", reinterpret_cast<void *>(load_library_ex_w_hook),
            reinterpret_cast<void **>(&g_load_library_ex_w)},
        {"GetModuleHandleA", reinterpret_cast<void *>(get_module_handle_a_hook),
            reinterpret_cast<void **>(&g_get_module_handle_a)},
        {"GetModuleHandleW", reinterpret_cast<void *>(get_module_handle_w_hook),
            reinterpret_cast<void **>(&g_get_module_handle_w)},
    }};
}

bool replace_import_slot(ULONG_PTR *slot, ImportReplacement &replacement) {
    void *current = reinterpret_cast<void *>(*slot);
    if (current == replacement.replacement) {
        return false;
    }
    if (!*replacement.original) {
        *replacement.original = current;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) {
        return false;
    }
    *slot = reinterpret_cast<ULONG_PTR>(replacement.replacement);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
    return true;
}

bool should_patch_module(HMODULE module) {
    if (module == GetModuleHandleW(nullptr)) {
        return true;
    }
    const std::wstring path = module_path(module);
    const wchar_t *name = base_name(path.c_str());
    if (!name || !name[0]) {
        return false;
    }
    static constexpr std::array<const wchar_t *, 6> candidates{{
        L"unityplayer.dll",
        L"mono.dll",
        L"mono-2.0-bdwgc.dll",
        L"gameassembly.dll",
        L"execexe.dll",
        L"kamunity.dll",
    }};
    return std::any_of(candidates.begin(), candidates.end(),
        [name](const wchar_t *candidate) {
            return _wcsicmp(name, candidate) == 0;
        });
}

bool patch_module_imports(HMODULE module) {
    if (!module || module == g_self || module == g_payload ||
        !should_patch_module(module)) {
        return false;
    }

    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY &directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) {
        return false;
    }

    bool patched = false;
    auto replacements = import_replacements();
    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
        base + directory.VirtualAddress);

    for (; descriptor->Name; ++descriptor) {
        const char *library = reinterpret_cast<const char *>(base + descriptor->Name);
        if (_stricmp(library, "kernel32.dll") != 0 &&
            _stricmp(library, "kernelbase.dll") != 0) {
            continue;
        }
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) {
            continue;
        }

        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(
            base + descriptor->OriginalFirstThunk);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(
            base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto *import = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
                base + names->u1.AddressOfData);
            for (auto &replacement : replacements) {
                if (strcmp(reinterpret_cast<const char *>(import->Name),
                           replacement.name) == 0) {
                    patched |= replace_import_slot(
                        reinterpret_cast<ULONG_PTR *>(&slots->u1.Function), replacement);
                    break;
                }
            }
        }
    }
    return patched;
}

size_t patch_loaded_modules() {
    size_t count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (patch_module_imports(entry.hModule)) {
                ++count;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

#if defined(ANYGEAR_BACKEND_KINECT)
void set_environment_default(const wchar_t *name, const wchar_t *value) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        SetEnvironmentVariableW(name, value);
    }
}
#endif

void configure_backend() {
    SetEnvironmentVariableA("VP4U_BACKEND", kBackendName);
#if defined(ANYGEAR_BACKEND_KINECT)
    set_environment_default(L"VP4U_CAPTURE_FPS", L"30");
    set_environment_default(L"VP4U_PREVIEW_FPS", L"15");
    set_environment_default(L"VP4U_IMAGE_POOL_MAX", L"12");
#endif
}

bool validate_payload(HMODULE payload) {
    static constexpr std::array<const char *, 4> exports{{
        "vp4uGetVersion",
        "vp4uPreboot",
        "vp4uShutdown",
        "vp4uGetApiTable",
    }};
    for (const char *name : exports) {
        if (!GetProcAddress(payload, name)) {
            toast_error(std::string("VP4U payload is missing export: ") + name);
            return false;
        }
    }
    return true;
}

std::string narrow_acp(const std::wstring &value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_ACP, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    result.pop_back();
    return result;
}

bool register_spice_alias(const std::string &name) {
    return g_spice.hook_library &&
        g_spice.hook_library(name.c_str(), g_payload) == ANYGEAR_SPICE_SUCCESS;
}

bool install_redirect() {
    if (g_spice.hook_library) {
        bool ok = true;
        ok &= register_spice_alias("visionposewrapper.dll");
        ok &= register_spice_alias("visionposewrapper");
        ok &= register_spice_alias("VisionPoseWrapper.dll");

        const std::wstring executable = module_path(nullptr);
        const std::wstring game_directory = parent_path(executable);
        const std::wstring absolute = game_directory +
            L"\\dancearound_data\\plugins\\x86_64\\visionposewrapper.dll";
        ok &= register_spice_alias(narrow_acp(absolute));
        ok &= register_spice_alias(narrow_acp(
            game_directory +
            L"\\dancearound_data\\plugins\\x86_64\\VisionPoseWrapper.dll"));
        if (ok) {
            log_message(ANYGEAR_SPICE_LOG_INFO,
                "registered VP4U aliases through Spice SDK");
            return true;
        } else {
            log_message(ANYGEAR_SPICE_LOG_WARNING,
                "Spice library alias registration was incomplete; enabling IAT fallback");
        }
    }

    const size_t patched = patch_loaded_modules();
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "installed VP4U IAT redirect in " + std::to_string(patched) + " module(s)");
    return patched != 0;
}

void __cdecl destroy_callback() {
    // Spice calls this after game entry returns. IAT slots can belong to modules
    // already being torn down, so process-exit cleanup is intentionally left to
    // Windows. The payload itself receives vp4uShutdown from the compatibility layer.
    log_message(ANYGEAR_SPICE_LOG_INFO,
        std::string("backend shutdown: ") + kBackendName);
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl spice_sdk_entry_point(
    anygear_spice_init_func init) {
    if (!init) {
        return 0;
    }

    g_spice = {};
    g_spice.size = sizeof(g_spice);
    const ANYGEAR_SPICE_STATUS status = init(0, destroy_callback, &g_spice);
    if (status != ANYGEAR_SPICE_SUCCESS) {
        return 0;
    }

    g_payload = g_self;
    log_message(ANYGEAR_SPICE_LOG_INFO,
        std::string("stage 1/4: Anygear ") + ANYGEAR_VERSION +
        " loaded; embedded VP4U backend=" + kBackendName);
    configure_backend();
#if defined(ANYGEAR_BACKEND_KINECT)
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "stage 2/4: stable Kinect profile selected "
        "(skeleton-only, 30 Hz, preview 15 Hz, image pool 12)");
#else
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "stage 2/4: MediaPipe USB webcam profile selected "
        "(CPU video tracking, 30 Hz, preview 15 Hz, image pool 12)");
#endif
    if (!validate_payload(g_payload)) {
        return 0;
    }
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "stage 3/4: embedded VP4U ABI 210 exports validated");
    if (!install_redirect()) {
        toast_error("could not install the visionposewrapper.dll redirect");
        return 0;
    }

    log_message(ANYGEAR_SPICE_LOG_INFO,
        std::string("stage 4/4: backend ready; waiting for Unity VP4U calls (") +
        kBackendName + ")");
    return 1;
}

extern "C" __declspec(dllexport) const char *__cdecl
danceAroundAnygearGetBuildInfo() {
    static const std::string build_info =
        std::string("dance-around-anygear/") + ANYGEAR_VERSION +
        ";vp4u=210;layout=monolithic;debug=dwarf-embedded;backend=" +
        kBackendName;
    return build_info.c_str();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
