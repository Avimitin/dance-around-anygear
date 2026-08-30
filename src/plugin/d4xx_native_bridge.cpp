// SPDX-License-Identifier: GPL-3.0-or-later
// Spice -k entry point for the native D4xx path. The game-owned VP4U and
// VisionPose binaries remain authoritative. This module adapts the public
// librealsense frame contract expected by the D435 wrapper and presents a
// realtime-only VP4U API table when recorded-video analysis is unavailable.

#include "anygear/spice_sdk.h"
#include "d4xx/ir_bgr.h"
#include "vp4u/vp4u_abi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include <librealsense2/rs.h>
#include <librealsense2/h/rs_config.h>
#include <librealsense2/h/rs_frame.h>
#include <librealsense2/h/rs_processing.h>
#include <librealsense2/h/rs_sensor.h>

#include <nlohmann/json.hpp>

#ifndef ANYGEAR_VERSION
#define ANYGEAR_VERSION "0.0.0-dev"
#endif

namespace {

using Json = nlohmann::json;
using LoadLibraryAFn = HMODULE (WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE (WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE (WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetModuleHandleAFn = HMODULE (WINAPI*)(LPCSTR);
using GetModuleHandleWFn = HMODULE (WINAPI*)(LPCWSTR);
using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);

struct BridgeConfig {
    int infrared_index = 1;
    anygear::d4xx::IrBgrOptions image;
    std::filesystem::path nvidia_runtime_root;
    std::filesystem::path cuda_root;
    std::filesystem::path diagnostic_frame_directory;
    unsigned int diagnostic_frame_count = 0;
    unsigned int diagnostic_frame_stride = 1;
    bool realtime_only = true;
};

struct FrameDescription {
    int width = 0;
    int height = 0;
    int stride = 0;
};

HMODULE g_self = nullptr;
HMODULE g_preloaded_wrapper = nullptr;
HMODULE g_original_wrapper = nullptr;
std::vector<HMODULE> g_nvidia_runtime_modules;
ANYGEAR_SPICE_SDK_V0 g_spice{};
BridgeConfig g_config{};
std::once_flag g_config_once;

decltype(&vp4u::vp4uGetVersion) g_original_get_version = nullptr;
decltype(&vp4u::vp4uPreboot) g_original_preboot = nullptr;
decltype(&vp4u::vp4uShutdown) g_original_shutdown = nullptr;
decltype(&vp4u::vp4uGetApiTable) g_original_get_api_table = nullptr;

enum class WrapperState : unsigned int {
    waiting,
    patched,
    incompatible,
};

std::atomic<WrapperState> g_wrapper_state{WrapperState::waiting};
void* g_loader_notification_cookie = nullptr;
HMODULE g_vp4u_export_module = nullptr;
DWORD* g_vp4u_export_slot = nullptr;
DWORD g_vp4u_original_export_rva = 0;
void* g_vp4u_export_thunk = nullptr;

decltype(&rs2_config_enable_stream) g_config_enable_stream = nullptr;
decltype(&rs2_create_align) g_create_align = nullptr;
decltype(&rs2_load_json) g_load_json = nullptr;
decltype(&rs2_is_sensor_extendable_to) g_is_sensor_extendable_to = nullptr;
decltype(&rs2_get_stream_profile_data) g_get_stream_profile_data = nullptr;
decltype(&rs2_get_frame_stream_profile) g_get_frame_stream_profile = nullptr;
decltype(&rs2_get_frame_data) g_get_frame_data = nullptr;
decltype(&rs2_get_frame_data_size) g_get_frame_data_size = nullptr;
decltype(&rs2_get_frame_width) g_get_frame_width = nullptr;
decltype(&rs2_get_frame_height) g_get_frame_height = nullptr;
decltype(&rs2_get_frame_stride_in_bytes) g_get_frame_stride = nullptr;
decltype(&rs2_get_frame_bits_per_pixel) g_get_frame_bits = nullptr;
decltype(&rs2_release_frame) g_release_frame = nullptr;
decltype(&rs2_free_error) g_free_error = nullptr;

std::mutex g_frame_mutex;
std::unordered_map<const rs2_frame*,
                   std::shared_ptr<std::vector<std::uint8_t>>> g_frame_buffers;
std::atomic<bool> g_logged_color_request{false};
std::atomic<bool> g_logged_json_filter{false};
std::atomic<bool> g_logged_sensor_alias{false};
std::atomic<bool> g_logged_frame{false};
std::atomic<bool> g_logged_api_table{false};
std::atomic<bool> g_logged_loader_redirect{false};
std::atomic<std::uint64_t> g_diagnostic_frame_sequence{0};
std::atomic<unsigned int> g_diagnostic_frame_index{0};

LoadLibraryAFn g_load_library_a = nullptr;
LoadLibraryWFn g_load_library_w = nullptr;
LoadLibraryExAFn g_load_library_ex_a = nullptr;
LoadLibraryExWFn g_load_library_ex_w = nullptr;
GetModuleHandleAFn g_get_module_handle_a = nullptr;
GetModuleHandleWFn g_get_module_handle_w = nullptr;
GetProcAddressFn g_get_proc_address = nullptr;
thread_local bool g_inside_loader_hook = false;

void log_message(ANYGEAR_SPICE_LOG_LEVEL level, const std::string& message) {
    if (g_spice.log) {
        g_spice.log(level, "dance_around_anygear_d4xx", message.c_str());
        return;
    }
    const std::string line = "[dance-around-anygear:d4xx] " + message + "\n";
    OutputDebugStringA(line.c_str());
}

void report_error(const std::string& message) {
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
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring parent_path(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::string narrow_acp(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_ACP, 0, value.c_str(), -1, result.data(), required,
        nullptr, nullptr);
    result.pop_back();
    return result;
}

const char* base_name(const char* path) {
    if (!path) return nullptr;
    const char* result = path;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') result = cursor + 1;
    }
    return result;
}

const wchar_t* base_name(const wchar_t* path) {
    if (!path) return nullptr;
    const wchar_t* result = path;
    for (const wchar_t* cursor = path; *cursor; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') result = cursor + 1;
    }
    return result;
}

bool is_vp4u_name(const char* path) {
    const char* name = base_name(path);
    return name && (_stricmp(name, "visionposewrapper.dll") == 0 ||
                    _stricmp(name, "visionposewrapper") == 0);
}

bool is_vp4u_name(const wchar_t* path) {
    const wchar_t* name = base_name(path);
    return name && (_wcsicmp(name, L"visionposewrapper.dll") == 0 ||
                    _wcsicmp(name, L"visionposewrapper") == 0);
}

HMODULE self_reference() {
    HMODULE referenced = nullptr;
    if (g_self && GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(g_self), &referenced)) {
        return referenced;
    }
    return g_self;
}

HMODULE redirected_vp4u_reference() {
    if (!g_logged_loader_redirect.exchange(true)) {
        log_message(ANYGEAR_SPICE_LOG_INFO,
            "loader compatibility redirected a VP4U module request");
    }
    return self_reference();
}

bool patch_loader_imports(HMODULE module);

struct LoaderScope {
    bool outer = false;
    LoaderScope() {
        outer = !g_inside_loader_hook;
        g_inside_loader_hook = true;
    }
    ~LoaderScope() {
        if (outer) g_inside_loader_hook = false;
    }
};

HMODULE WINAPI load_library_a_hook(LPCSTR file_name) {
    if (is_vp4u_name(file_name)) return redirected_vp4u_reference();
    if (!g_load_library_a) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_a(file_name);
    if (scope.outer && module) patch_loader_imports(module);
    return module;
}

HMODULE WINAPI load_library_w_hook(LPCWSTR file_name) {
    if (is_vp4u_name(file_name)) return redirected_vp4u_reference();
    if (!g_load_library_w) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_w(file_name);
    if (scope.outer && module) patch_loader_imports(module);
    return module;
}

HMODULE WINAPI load_library_ex_a_hook(
    LPCSTR file_name, HANDLE file, DWORD flags) {
    if (is_vp4u_name(file_name)) return redirected_vp4u_reference();
    if (!g_load_library_ex_a) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_ex_a(file_name, file, flags);
    if (scope.outer && module) patch_loader_imports(module);
    return module;
}

HMODULE WINAPI load_library_ex_w_hook(
    LPCWSTR file_name, HANDLE file, DWORD flags) {
    if (is_vp4u_name(file_name)) return redirected_vp4u_reference();
    if (!g_load_library_ex_w) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    LoaderScope scope;
    HMODULE module = g_load_library_ex_w(file_name, file, flags);
    if (scope.outer && module) patch_loader_imports(module);
    return module;
}

HMODULE WINAPI get_module_handle_a_hook(LPCSTR module_name) {
    if (is_vp4u_name(module_name)) return g_self;
    return g_get_module_handle_a ? g_get_module_handle_a(module_name) : nullptr;
}

HMODULE WINAPI get_module_handle_w_hook(LPCWSTR module_name) {
    if (is_vp4u_name(module_name)) return g_self;
    return g_get_module_handle_w ? g_get_module_handle_w(module_name) : nullptr;
}

bool is_vp4u_export_name(LPCSTR procedure_name) {
    if (!procedure_name ||
        reinterpret_cast<ULONG_PTR>(procedure_name) <= 0xffffu) {
        return false;
    }
    static constexpr std::array<const char*, 4> exports{{
        "vp4uGetVersion", "vp4uPreboot", "vp4uShutdown", "vp4uGetApiTable",
    }};
    return std::any_of(
        exports.begin(), exports.end(),
        [procedure_name](const char* candidate) {
            return std::strcmp(procedure_name, candidate) == 0;
        });
}

bool is_original_vp4u_module(HMODULE module) {
    if (!module) return false;
    if (module == g_original_wrapper || module == g_preloaded_wrapper) {
        return true;
    }
    const std::wstring path = module_path(module);
    return is_vp4u_name(path.c_str());
}

FARPROC WINAPI get_proc_address_hook(
    HMODULE module, LPCSTR procedure_name) {
    if (g_get_proc_address && is_original_vp4u_module(module) &&
        is_vp4u_export_name(procedure_name)) {
        FARPROC redirected = g_get_proc_address(g_self, procedure_name);
        if (redirected) {
            if (!g_logged_loader_redirect.exchange(true)) {
                log_message(ANYGEAR_SPICE_LOG_INFO,
                    "loader compatibility redirected cached VP4U exports");
            }
            return redirected;
        }
    }
    return g_get_proc_address
        ? g_get_proc_address(module, procedure_name)
        : nullptr;
}

struct LoaderImportReplacement {
    const char* name;
    void* replacement;
    void** original;
};

std::array<LoaderImportReplacement, 7> loader_import_replacements() {
    return {{{"LoadLibraryA", reinterpret_cast<void*>(load_library_a_hook),
              reinterpret_cast<void**>(&g_load_library_a)},
             {"LoadLibraryW", reinterpret_cast<void*>(load_library_w_hook),
              reinterpret_cast<void**>(&g_load_library_w)},
             {"LoadLibraryExA", reinterpret_cast<void*>(load_library_ex_a_hook),
              reinterpret_cast<void**>(&g_load_library_ex_a)},
             {"LoadLibraryExW", reinterpret_cast<void*>(load_library_ex_w_hook),
              reinterpret_cast<void**>(&g_load_library_ex_w)},
             {"GetModuleHandleA",
              reinterpret_cast<void*>(get_module_handle_a_hook),
              reinterpret_cast<void**>(&g_get_module_handle_a)},
             {"GetModuleHandleW",
              reinterpret_cast<void*>(get_module_handle_w_hook),
              reinterpret_cast<void**>(&g_get_module_handle_w)},
             {"GetProcAddress",
              reinterpret_cast<void*>(get_proc_address_hook),
              reinterpret_cast<void**>(&g_get_proc_address)}}};
}

bool replace_loader_import_slot(
    ULONG_PTR* slot, LoaderImportReplacement& replacement) {
    const void* current = reinterpret_cast<void*>(*slot);
    if (current == replacement.replacement) return false;
    if (!*replacement.original) {
        *replacement.original = reinterpret_cast<void*>(*slot);
    }
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
        return false;
    }
    *slot = reinterpret_cast<ULONG_PTR>(replacement.replacement);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    return true;
}

bool should_patch_loader_imports(HMODULE module) {
    if (module == GetModuleHandleW(nullptr)) return true;
    const std::wstring path = module_path(module);
    const wchar_t* name = base_name(path.c_str());
    if (!name || !name[0]) return false;
    static constexpr std::array<const wchar_t*, 6> candidates{{
        L"unityplayer.dll", L"mono.dll", L"mono-2.0-bdwgc.dll",
        L"gameassembly.dll", L"execexe.dll", L"kamunity.dll",
    }};
    return std::any_of(
        candidates.begin(), candidates.end(), [name](const wchar_t* candidate) {
            return _wcsicmp(name, candidate) == 0;
        });
}

bool patch_loader_imports(HMODULE module) {
    if (!module || module == g_self || !should_patch_loader_imports(module)) {
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return false;

    bool patched = false;
    auto replacements = loader_import_replacements();
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* library =
            reinterpret_cast<const char*>(base + descriptor->Name);
        if ((_stricmp(library, "kernel32.dll") != 0 &&
             _stricmp(library, "kernelbase.dll") != 0) ||
            !descriptor->OriginalFirstThunk || !descriptor->FirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            for (auto& replacement : replacements) {
                if (std::strcmp(
                        reinterpret_cast<const char*>(imported->Name),
                        replacement.name) == 0) {
                    patched |= replace_loader_import_slot(
                        reinterpret_cast<ULONG_PTR*>(&slots->u1.Function),
                        replacement);
                    break;
                }
            }
        }
    }
    return patched;
}

std::size_t patch_loaded_loader_imports() {
    std::size_t count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (patch_loader_imports(entry.hModule)) ++count;
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

void load_config() {
    g_config = {};
    const std::filesystem::path path =
        std::filesystem::path(parent_path(module_path(g_self))) /
        L"dance_around_anygear_d4xx.json";
    std::ifstream input(path);
    if (input) {
        try {
            Json document;
            input >> document;
            if (const auto found = document.find("D4xxNativeBridge");
                found != document.end() && found->is_object()) {
                const Json& section = *found;
                if (const auto value = section.find("InfraredIndex");
                    value != section.end() && value->is_number_integer()) {
                    g_config.infrared_index = value->get<int>();
                }
                if (const auto value = section.find("AutoContrast");
                    value != section.end() && value->is_boolean()) {
                    g_config.image.auto_contrast = value->get<bool>();
                }
                if (const auto value = section.find("LowPercentile");
                    value != section.end() && value->is_number_unsigned()) {
                    g_config.image.low_percentile = value->get<unsigned int>();
                }
                if (const auto value = section.find("HighPercentile");
                    value != section.end() && value->is_number_unsigned()) {
                    g_config.image.high_percentile = value->get<unsigned int>();
                }
                if (const auto value = section.find("MinimumRange");
                    value != section.end() && value->is_number_integer()) {
                    g_config.image.minimum_range = value->get<int>();
                }
                if (const auto value = section.find("RealtimeOnly");
                    value != section.end() && value->is_boolean()) {
                    g_config.realtime_only = value->get<bool>();
                }
                const std::filesystem::path config_directory = path.parent_path();
                const auto read_path = [&section, &config_directory](
                                           const char* name) {
                    const auto value = section.find(name);
                    if (value == section.end() || !value->is_string() ||
                        value->get_ref<const std::string&>().empty()) {
                        return std::filesystem::path{};
                    }
                    std::filesystem::path result = std::filesystem::u8path(
                        value->get_ref<const std::string&>());
                    if (result.is_relative()) result = config_directory / result;
                    return result.lexically_normal();
                };
                g_config.nvidia_runtime_root = read_path("NvidiaRuntimeRoot");
                g_config.cuda_root = read_path("CudaRoot");
                g_config.diagnostic_frame_directory =
                    read_path("DiagnosticFrameDirectory");
                if (const auto value = section.find("DiagnosticFrameCount");
                    value != section.end() && value->is_number_unsigned()) {
                    g_config.diagnostic_frame_count = value->get<unsigned int>();
                }
                if (const auto value = section.find("DiagnosticFrameStride");
                    value != section.end() && value->is_number_unsigned()) {
                    g_config.diagnostic_frame_stride = value->get<unsigned int>();
                }
            }
        } catch (const std::exception& error) {
            log_message(ANYGEAR_SPICE_LOG_WARNING,
                std::string("configuration ignored: ") + error.what());
        }
    }
    g_config.infrared_index = std::clamp(g_config.infrared_index, 1, 2);
    g_config.image.low_percentile =
        std::min(g_config.image.low_percentile, 98u);
    g_config.image.high_percentile = std::clamp(
        g_config.image.high_percentile,
        g_config.image.low_percentile + 1, 100u);
    g_config.image.minimum_range =
        std::clamp(g_config.image.minimum_range, 1, 255);
    g_config.diagnostic_frame_count =
        std::min(g_config.diagnostic_frame_count, 120u);
    g_config.diagnostic_frame_stride =
        std::clamp(g_config.diagnostic_frame_stride, 1u, 3600u);
}

void ensure_config() {
    std::call_once(g_config_once, load_config);
}

void discard_error(rs2_error* error) {
    if (error && g_free_error) g_free_error(error);
}

bool is_virtual_ir_profile(const rs2_stream_profile* profile) {
    ensure_config();
    if (!profile || !g_get_stream_profile_data) return false;
    rs2_stream stream = RS2_STREAM_ANY;
    rs2_format format = RS2_FORMAT_ANY;
    int index = -1;
    int unique_id = 0;
    int framerate = 0;
    rs2_error* error = nullptr;
    g_get_stream_profile_data(
        profile, &stream, &format, &index, &unique_id, &framerate, &error);
    if (error) {
        discard_error(error);
        return false;
    }
    return stream == RS2_STREAM_INFRARED && format == RS2_FORMAT_Y8 &&
           index == g_config.infrared_index;
}

bool describe_virtual_frame(
    const rs2_frame* frame, FrameDescription* output) {
    if (!frame || !output || !g_get_frame_stream_profile ||
        !g_get_frame_width || !g_get_frame_height || !g_get_frame_stride) {
        return false;
    }
    rs2_error* error = nullptr;
    const rs2_stream_profile* profile =
        g_get_frame_stream_profile(frame, &error);
    if (error) {
        discard_error(error);
        return false;
    }
    if (!is_virtual_ir_profile(profile)) return false;

    output->width = g_get_frame_width(frame, &error);
    if (error) {
        discard_error(error);
        return false;
    }
    output->height = g_get_frame_height(frame, &error);
    if (error) {
        discard_error(error);
        return false;
    }
    output->stride = g_get_frame_stride(frame, &error);
    if (error) {
        discard_error(error);
        return false;
    }
    return output->width > 0 && output->height > 0 &&
           output->stride >= output->width;
}

bool write_bgr24_bmp(const std::filesystem::path& path,
                     const std::vector<std::uint8_t>& pixels,
                     int width,
                     int height) {
    if (width <= 0 || height <= 0) return false;
    const std::size_t source_stride = static_cast<std::size_t>(width) * 3;
    const std::size_t output_stride = (source_stride + 3u) & ~std::size_t{3u};
    const std::size_t image_bytes = output_stride *
        static_cast<std::size_t>(height);
    if (pixels.size() < source_stride * static_cast<std::size_t>(height) ||
        image_bytes > std::numeric_limits<DWORD>::max()) {
        return false;
    }

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    file_header.bfType = 0x4d42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits +
        static_cast<DWORD>(image_bytes);
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = width;
    info_header.biHeight = -height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 24;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = static_cast<DWORD>(image_bytes);

    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(&file_header),
                 sizeof(file_header));
    output.write(reinterpret_cast<const char*>(&info_header),
                 sizeof(info_header));
    const std::array<std::uint8_t, 3> padding{};
    for (int y = 0; y < height; ++y) {
        const auto* row = pixels.data() +
            static_cast<std::size_t>(y) * source_stride;
        output.write(reinterpret_cast<const char*>(row), source_stride);
        output.write(reinterpret_cast<const char*>(padding.data()),
                     output_stride - source_stride);
    }
    return output.good();
}

void capture_diagnostic_frame(const std::vector<std::uint8_t>& pixels,
                              const FrameDescription& description,
                              const anygear::d4xx::IrBgrStats& stats) {
    if (g_config.diagnostic_frame_directory.empty() ||
        g_config.diagnostic_frame_count == 0) {
        return;
    }
    const std::uint64_t sequence =
        g_diagnostic_frame_sequence.fetch_add(1, std::memory_order_relaxed);
    const unsigned int burst =
        std::min(2u, g_config.diagnostic_frame_stride);
    if (sequence % g_config.diagnostic_frame_stride >= burst) return;
    const unsigned int index =
        g_diagnostic_frame_index.fetch_add(1, std::memory_order_relaxed);
    if (index >= g_config.diagnostic_frame_count) return;

    std::error_code directory_error;
    std::filesystem::create_directories(
        g_config.diagnostic_frame_directory, directory_error);
    if (directory_error) {
        if (index == 0) {
            log_message(ANYGEAR_SPICE_LOG_WARNING,
                "diagnostic frame directory could not be created: " +
                directory_error.message());
        }
        return;
    }

    const std::string filename =
        "visionpose-input-" + std::to_string(index) + "-" +
        std::to_string(description.width) + "x" +
        std::to_string(description.height) + "-tone-" +
        std::to_string(stats.low) + "-" + std::to_string(stats.high) +
        ".bmp";
    const auto path = g_config.diagnostic_frame_directory /
        std::filesystem::u8path(filename);
    if (!write_bgr24_bmp(path, pixels, description.width,
                         description.height)) {
        log_message(ANYGEAR_SPICE_LOG_WARNING,
            "diagnostic frame could not be written: " + path.u8string());
        return;
    }
    if (index + 1 == g_config.diagnostic_frame_count) {
        log_message(ANYGEAR_SPICE_LOG_INFO,
            "diagnostic frame capture complete: " +
            g_config.diagnostic_frame_directory.u8string());
    }
}

void bridge_config_enable_stream(rs2_config* config,
                                 rs2_stream stream,
                                 int index,
                                 int width,
                                 int height,
                                 rs2_format format,
                                 int framerate,
                                 rs2_error** error) {
    ensure_config();
    if (stream == RS2_STREAM_COLOR && format == RS2_FORMAT_BGR8) {
        if (!g_logged_color_request.exchange(true)) {
            log_message(ANYGEAR_SPICE_LOG_INFO,
                "camera contract: COLOR/BGR8 request mapped to IR/Y8 index " +
                std::to_string(g_config.infrared_index));
        }
        g_config_enable_stream(
            config, RS2_STREAM_INFRARED, g_config.infrared_index,
            width, height, RS2_FORMAT_Y8, framerate, error);
        return;
    }
    g_config_enable_stream(
        config, stream, index, width, height, format, framerate, error);
}

rs2_processing_block* bridge_create_align(
    rs2_stream align_to, rs2_error** error) {
    ensure_config();
    if (align_to == RS2_STREAM_COLOR) {
        align_to = RS2_STREAM_INFRARED;
    }
    return g_create_align(align_to, error);
}

std::size_t remove_color_sensor_controls(Json& value) {
    std::size_t removed = 0;
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end();) {
            if (item.key().rfind("controls-color-", 0) == 0) {
                item = value.erase(item);
                ++removed;
            } else {
                removed += remove_color_sensor_controls(item.value());
                ++item;
            }
        }
    } else if (value.is_array()) {
        for (Json& item : value) {
            removed += remove_color_sensor_controls(item);
        }
    }
    return removed;
}

void bridge_load_json(rs2_device* device,
                      const void* json_content,
                      unsigned int content_size,
                      rs2_error** error) {
    if (!g_load_json || !json_content || content_size == 0) {
        if (g_load_json) {
            g_load_json(device, json_content, content_size, error);
        }
        return;
    }
    const auto* begin = static_cast<const char*>(json_content);
    const char* end = begin + content_size;
    while (end > begin && end[-1] == '\0') --end;
    try {
        Json document = Json::parse(begin, end);
        const std::size_t removed = remove_color_sensor_controls(document);
        if (removed != 0) {
            const std::string filtered = document.dump();
            if (!g_logged_json_filter.exchange(true)) {
                log_message(ANYGEAR_SPICE_LOG_INFO,
                    "camera contract: ignored " +
                    std::to_string(removed) +
                    " RGB-only advanced camera setting(s)");
            }
            g_load_json(
                device, filtered.data(),
                static_cast<unsigned int>(filtered.size()), error);
            return;
        }
    } catch (const std::exception& exception) {
        log_message(ANYGEAR_SPICE_LOG_WARNING,
            std::string("advanced camera JSON was not filtered: ") +
            exception.what());
    }
    g_load_json(device, json_content, content_size, error);
}

int bridge_is_sensor_extendable_to(const rs2_sensor* sensor,
                                   rs2_extension extension,
                                   rs2_error** error) {
    const int supported =
        g_is_sensor_extendable_to(sensor, extension, error);
    if (supported || extension != RS2_EXTENSION_COLOR_SENSOR ||
        (error && *error)) {
        return supported;
    }

    rs2_error* depth_error = nullptr;
    const int is_depth = g_is_sensor_extendable_to(
        sensor, RS2_EXTENSION_DEPTH_SENSOR, &depth_error);
    if (depth_error) {
        discard_error(depth_error);
        return supported;
    }
    if (is_depth && !g_logged_sensor_alias.exchange(true)) {
        log_message(ANYGEAR_SPICE_LOG_INFO,
            "camera contract: depth sensor supplies the virtual color "
            "sensor role");
    }
    return is_depth ? 1 : supported;
}

void bridge_get_stream_profile_data(const rs2_stream_profile* mode,
                                    rs2_stream* stream,
                                    rs2_format* format,
                                    int* index,
                                    int* unique_id,
                                    int* framerate,
                                    rs2_error** error) {
    ensure_config();
    g_get_stream_profile_data(
        mode, stream, format, index, unique_id, framerate, error);
    if (error && *error) return;
    if (stream && format && *stream == RS2_STREAM_INFRARED &&
        *format == RS2_FORMAT_Y8 && index &&
        *index == g_config.infrared_index) {
        *stream = RS2_STREAM_COLOR;
        *format = RS2_FORMAT_BGR8;
        *index = 0;
    }
}

const void* bridge_get_frame_data(
    const rs2_frame* frame, rs2_error** error) {
    FrameDescription description;
    if (!describe_virtual_frame(frame, &description)) {
        return g_get_frame_data(frame, error);
    }

    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        const auto found = g_frame_buffers.find(frame);
        if (found != g_frame_buffers.end()) {
            return found->second->data();
        }
    }

    const int source_bytes = g_get_frame_data_size(frame, error);
    if ((error && *error) || source_bytes <= 0) return nullptr;
    const void* source = g_get_frame_data(frame, error);
    if ((error && *error) || !source) return nullptr;

    auto converted =
        std::make_shared<std::vector<std::uint8_t>>();
    anygear::d4xx::IrBgrStats stats;
    if (!anygear::d4xx::ir_y8_to_bgr24(
            static_cast<const std::uint8_t*>(source),
            static_cast<std::size_t>(source_bytes),
            description.width, description.height, description.stride,
            g_config.image, converted.get(), &stats)) {
        return nullptr;
    }
    if (!g_logged_frame.exchange(true)) {
        log_message(ANYGEAR_SPICE_LOG_INFO,
            "camera contract: first virtual BGR24 frame " +
            std::to_string(description.width) + "x" +
            std::to_string(description.height) + ", tone range=" +
            std::to_string(stats.low) + ".." + std::to_string(stats.high));
    }
    capture_diagnostic_frame(*converted, description, stats);
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    const auto inserted = g_frame_buffers.emplace(frame, std::move(converted));
    return inserted.first->second->data();
}

int bridge_get_frame_data_size(const rs2_frame* frame, rs2_error** error) {
    FrameDescription description;
    if (!describe_virtual_frame(frame, &description)) {
        return g_get_frame_data_size(frame, error);
    }
    return description.width * description.height * 3;
}

int bridge_get_frame_stride(const rs2_frame* frame, rs2_error** error) {
    FrameDescription description;
    if (!describe_virtual_frame(frame, &description)) {
        return g_get_frame_stride(frame, error);
    }
    return description.width * 3;
}

int bridge_get_frame_bits(const rs2_frame* frame, rs2_error** error) {
    FrameDescription description;
    if (!describe_virtual_frame(frame, &description)) {
        return g_get_frame_bits(frame, error);
    }
    return 24;
}

void bridge_release_frame(rs2_frame* frame) {
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_frame_buffers.erase(frame);
    }
    g_release_frame(frame);
}

struct ImportBinding {
    const char* name;
    void* replacement;
    void** original;
    ULONG_PTR* slot = nullptr;
};

std::array<ImportBinding, 14> bridge_bindings() {
    return {{
        {"rs2_config_enable_stream",
         reinterpret_cast<void*>(bridge_config_enable_stream),
         reinterpret_cast<void**>(&g_config_enable_stream)},
        {"rs2_create_align", reinterpret_cast<void*>(bridge_create_align),
         reinterpret_cast<void**>(&g_create_align)},
        {"rs2_load_json", reinterpret_cast<void*>(bridge_load_json),
         reinterpret_cast<void**>(&g_load_json)},
        {"rs2_is_sensor_extendable_to",
         reinterpret_cast<void*>(bridge_is_sensor_extendable_to),
         reinterpret_cast<void**>(&g_is_sensor_extendable_to)},
        {"rs2_get_stream_profile_data",
         reinterpret_cast<void*>(bridge_get_stream_profile_data),
         reinterpret_cast<void**>(&g_get_stream_profile_data)},
        {"rs2_get_frame_stream_profile", nullptr,
         reinterpret_cast<void**>(&g_get_frame_stream_profile)},
        {"rs2_get_frame_data", reinterpret_cast<void*>(bridge_get_frame_data),
         reinterpret_cast<void**>(&g_get_frame_data)},
        {"rs2_get_frame_data_size",
         reinterpret_cast<void*>(bridge_get_frame_data_size),
         reinterpret_cast<void**>(&g_get_frame_data_size)},
        {"rs2_get_frame_width", nullptr,
         reinterpret_cast<void**>(&g_get_frame_width)},
        {"rs2_get_frame_height", nullptr,
         reinterpret_cast<void**>(&g_get_frame_height)},
        {"rs2_get_frame_stride_in_bytes",
         reinterpret_cast<void*>(bridge_get_frame_stride),
         reinterpret_cast<void**>(&g_get_frame_stride)},
        {"rs2_get_frame_bits_per_pixel",
         reinterpret_cast<void*>(bridge_get_frame_bits),
         reinterpret_cast<void**>(&g_get_frame_bits)},
        {"rs2_release_frame", reinterpret_cast<void*>(bridge_release_frame),
         reinterpret_cast<void**>(&g_release_frame)},
        {"rs2_free_error", nullptr,
         reinterpret_cast<void**>(&g_free_error)},
    }};
}

bool patch_realsense_imports(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return false;

    auto bindings = bridge_bindings();
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* library =
            reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(library, "realsense2.dll") != 0 ||
            !descriptor->OriginalFirstThunk || !descriptor->FirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(import->Name);
            for (auto& binding : bindings) {
                if (std::strcmp(name, binding.name) != 0) continue;
                *binding.original =
                    reinterpret_cast<void*>(slots->u1.Function);
                if (binding.replacement) {
                    binding.slot =
                        reinterpret_cast<ULONG_PTR*>(&slots->u1.Function);
                }
                break;
            }
        }
    }
    for (const auto& binding : bindings) {
        if (!*binding.original || (binding.replacement && !binding.slot)) {
            log_message(ANYGEAR_SPICE_LOG_WARNING,
                std::string("original wrapper import is absent: ") +
                binding.name);
            return false;
        }
    }
    for (const auto& binding : bindings) {
        if (!binding.replacement) continue;
        DWORD protection = 0;
        if (!VirtualProtect(
                binding.slot, sizeof(*binding.slot), PAGE_READWRITE,
                &protection)) {
            return false;
        }
        *binding.slot = reinterpret_cast<ULONG_PTR>(binding.replacement);
        FlushInstructionCache(
            GetCurrentProcess(), binding.slot, sizeof(*binding.slot));
        DWORD ignored = 0;
        VirtualProtect(
            binding.slot, sizeof(*binding.slot), protection, &ignored);
    }
    return true;
}

bool validate_original_wrapper(HMODULE module) {
    static constexpr std::array<const char*, 4> exports{{
        "vp4uGetVersion", "vp4uPreboot", "vp4uShutdown", "vp4uGetApiTable",
    }};
    return module && std::all_of(
        exports.begin(), exports.end(), [module](const char* name) {
            return GetProcAddress(module, name) != nullptr;
        });
}

struct LoaderUnicodeString {
    USHORT length;
    USHORT maximum_length;
    wchar_t* buffer;
};

struct LoaderDllData {
    ULONG flags;
    const LoaderUnicodeString* full_name;
    const LoaderUnicodeString* base_name;
    void* base;
    ULONG image_size;
};

union LoaderNotificationData {
    LoaderDllData loaded;
    LoaderDllData unloaded;
};

using LoaderNotificationCallback = void (CALLBACK*)(
    ULONG reason, const LoaderNotificationData* data, void* context);
using RegisterLoaderNotification = LONG (NTAPI*)(
    ULONG flags, LoaderNotificationCallback callback, void* context,
    void** cookie);
using UnregisterLoaderNotification = LONG (NTAPI*)(void* cookie);

RegisterLoaderNotification g_register_loader_notification = nullptr;
UnregisterLoaderNotification g_unregister_loader_notification = nullptr;

template <typename T>
T module_procedure(HMODULE module, const char* name) {
    const FARPROC raw = GetProcAddress(module, name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

bool bind_original_vp4u(HMODULE module) {
    if (!module) return false;
    const auto get_version =
        module_procedure<decltype(g_original_get_version)>(
            module, "vp4uGetVersion");
    const auto preboot = module_procedure<decltype(g_original_preboot)>(
        module, "vp4uPreboot");
    const auto shutdown = module_procedure<decltype(g_original_shutdown)>(
        module, "vp4uShutdown");
    const auto get_api_table =
        module_procedure<decltype(g_original_get_api_table)>(
            module, "vp4uGetApiTable");
    if (!get_version || !preboot || !shutdown || !get_api_table) {
        return false;
    }
    g_original_wrapper = module;
    g_original_get_version = get_version;
    g_original_preboot = preboot;
    g_original_shutdown = shutdown;
    g_original_get_api_table = get_api_table;
    return true;
}

DWORD* find_named_export_slot(HMODULE module, const char* export_name) {
    if (!module || !export_name) return nullptr;
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
        base + directory.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(
        base + exports->AddressOfNames);
    auto* ordinals = reinterpret_cast<WORD*>(
        base + exports->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<DWORD*>(
        base + exports->AddressOfFunctions);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const char* name = reinterpret_cast<const char*>(base + names[index]);
        if (std::strcmp(name, export_name) != 0) continue;
        const WORD ordinal = ordinals[index];
        return ordinal < exports->NumberOfFunctions
            ? &functions[ordinal]
            : nullptr;
    }
    return nullptr;
}

void* allocate_near_jump_thunk(HMODULE module, const void* target) {
#if defined(_WIN64)
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::uintptr_t granularity =
        system_info.dwAllocationGranularity;
    const auto align_up = [granularity](std::uintptr_t value) {
        return (value + granularity - 1) & ~(granularity - 1);
    };
    const std::uintptr_t module_base =
        reinterpret_cast<std::uintptr_t>(module);
    std::uintptr_t cursor = align_up(
        module_base + nt->OptionalHeader.SizeOfImage);
    const std::uintptr_t application_max =
        reinterpret_cast<std::uintptr_t>(
            system_info.lpMaximumApplicationAddress);
    const std::uintptr_t export_max = module_base + MAXDWORD;
    const std::uintptr_t last_address =
        std::min(application_max, export_max) - 16;

    while (cursor <= last_address) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(
                reinterpret_cast<const void*>(cursor), &memory,
                sizeof(memory))) {
            break;
        }
        const std::uintptr_t region_base =
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const std::uintptr_t region_end = region_base + memory.RegionSize;
        if (memory.State == MEM_FREE) {
            const std::uintptr_t candidate = align_up(
                std::max(cursor, region_base));
            if (candidate <= last_address && candidate + 16 <= region_end) {
                void* thunk = VirtualAlloc(
                    reinterpret_cast<void*>(candidate), 16,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (thunk) {
                    std::array<std::uint8_t, 12> code{{
                        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0,
                    }};
                    const std::uintptr_t destination =
                        reinterpret_cast<std::uintptr_t>(target);
                    std::memcpy(code.data() + 2, &destination,
                                sizeof(destination));
                    std::memcpy(thunk, code.data(), code.size());
                    FlushInstructionCache(
                        GetCurrentProcess(), thunk, code.size());
                    DWORD ignored = 0;
                    VirtualProtect(
                        thunk, 16, PAGE_EXECUTE_READ, &ignored);
                    return thunk;
                }
            }
        }
        if (region_end <= cursor) break;
        cursor = align_up(region_end);
    }
#else
    (void)module;
    (void)target;
#endif
    return nullptr;
}

bool install_vp4u_export_redirect(HMODULE module) {
    if (g_vp4u_export_slot) return g_vp4u_export_module == module;
    DWORD* slot = find_named_export_slot(module, "vp4uGetApiTable");
    const FARPROC bridge_export = GetProcAddress(g_self, "vp4uGetApiTable");
    if (!slot || !bridge_export) return false;

    const std::uintptr_t module_base =
        reinterpret_cast<std::uintptr_t>(module);
    std::uintptr_t target =
        reinterpret_cast<std::uintptr_t>(bridge_export);
    if (target < module_base || target - module_base > MAXDWORD) {
        g_vp4u_export_thunk = allocate_near_jump_thunk(
            module, reinterpret_cast<const void*>(target));
        if (!g_vp4u_export_thunk) return false;
        target = reinterpret_cast<std::uintptr_t>(g_vp4u_export_thunk);
    }

    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
        if (g_vp4u_export_thunk) {
            VirtualFree(g_vp4u_export_thunk, 0, MEM_RELEASE);
            g_vp4u_export_thunk = nullptr;
        }
        return false;
    }
    const DWORD original = *slot;
    *slot = static_cast<DWORD>(target - module_base);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);

    g_vp4u_export_module = module;
    g_vp4u_export_slot = slot;
    g_vp4u_original_export_rva = original;
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "VP4U API-table export redirected for cached native callers");
    return true;
}

void restore_vp4u_export_redirect() {
    if (g_vp4u_export_slot) {
        DWORD protection = 0;
        if (VirtualProtect(
                g_vp4u_export_slot, sizeof(*g_vp4u_export_slot),
                PAGE_READWRITE, &protection)) {
            *g_vp4u_export_slot = g_vp4u_original_export_rva;
            FlushInstructionCache(
                GetCurrentProcess(), g_vp4u_export_slot,
                sizeof(*g_vp4u_export_slot));
            DWORD ignored = 0;
            VirtualProtect(
                g_vp4u_export_slot, sizeof(*g_vp4u_export_slot),
                protection, &ignored);
        }
    }
    g_vp4u_export_module = nullptr;
    g_vp4u_export_slot = nullptr;
    g_vp4u_original_export_rva = 0;
    if (g_vp4u_export_thunk) {
        VirtualFree(g_vp4u_export_thunk, 0, MEM_RELEASE);
        g_vp4u_export_thunk = nullptr;
    }
}

int bridge_init_visionpose_offline(const char*) {
    return 1;
}

bool bridge_start_offline_analysis(
    const char*, vp4u::OnPoseFrameCallback, void*) {
    return true;
}

void bridge_stop_offline_analysis() {}

void adapt_vp4u_api_table(vp4u::FunctionPointerTable* table) {
    ensure_config();
    if (!table || !g_config.realtime_only) return;
    table->InitVisionPoseOffline = bridge_init_visionpose_offline;
    table->StartOfflineAnalysis = bridge_start_offline_analysis;
    table->StopOfflineAnalysis = bridge_stop_offline_analysis;
    if (!g_logged_api_table.exchange(true)) {
        log_message(ANYGEAR_SPICE_LOG_INFO,
            "VP4U API table: realtime camera analysis retained; "
            "recorded-video analysis is inactive");
    }
}

bool wrapper_name_matches(const LoaderUnicodeString* name) {
    static constexpr std::array<const wchar_t*, 2> expected{{
        L"visionposewrapper.dll", L"visionposewrapper.d435.dll",
    }};
    if (!name || !name->buffer) return false;
    for (const wchar_t* candidate : expected) {
        const std::size_t length = std::wcslen(candidate);
        if (name->length != length * sizeof(wchar_t)) continue;
        bool equal = true;
        for (std::size_t index = 0; index < length; ++index) {
            wchar_t actual = name->buffer[index];
            if (actual >= L'A' && actual <= L'Z') actual += L'a' - L'A';
            if (actual != candidate[index]) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

void CALLBACK loader_notification(
    ULONG reason, const LoaderNotificationData* data, void*) {
    static constexpr ULONG loaded_reason = 1;
    if (reason != loaded_reason || !data) return;
    HMODULE loaded_module = static_cast<HMODULE>(data->loaded.base);
    if (patch_loader_imports(loaded_module)) {
        const std::wstring path = module_path(loaded_module);
        const wchar_t* name = base_name(path.c_str());
        log_message(ANYGEAR_SPICE_LOG_INFO,
            "loader compatibility attached to " +
            narrow_acp(name ? name : L"unknown module"));
    }
    if (!wrapper_name_matches(data->loaded.base_name)) return;
    if (g_wrapper_state.load(std::memory_order_acquire) !=
        WrapperState::waiting) {
        return;
    }
    HMODULE wrapper = loaded_module;
    if (validate_original_wrapper(wrapper) && bind_original_vp4u(wrapper) &&
        patch_realsense_imports(wrapper) &&
        install_vp4u_export_redirect(wrapper)) {
        g_wrapper_state.store(WrapperState::patched,
                              std::memory_order_release);
    } else {
        g_wrapper_state.store(WrapperState::incompatible,
                              std::memory_order_release);
    }
}

bool register_wrapper_aliases() {
    if (g_spice.hook_library) {
        const std::wstring executable = module_path(nullptr);
        const std::wstring game_directory = parent_path(executable);
        const std::array<std::string, 5> aliases{{
            "visionposewrapper.dll",
            "visionposewrapper",
            "VisionPoseWrapper.dll",
            narrow_acp(game_directory +
                L"\\dancearound_data\\plugins\\x86_64\\visionposewrapper.dll"),
            narrow_acp(game_directory +
                L"\\dancearound_data\\plugins\\x86_64\\VisionPoseWrapper.dll"),
        }};
        for (const std::string& alias : aliases) {
            if (alias.empty() ||
                g_spice.hook_library(alias.c_str(), g_self) !=
                    ANYGEAR_SPICE_SUCCESS) {
                report_error(
                    "could not register the VP4U module alias: " + alias);
                return false;
            }
        }
        return true;
    }

    const std::size_t count = patch_loaded_loader_imports();
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "Spice SDK module aliases unavailable; installed loader "
        "compatibility in " + std::to_string(count) + " module(s)");
    return count != 0;
}

bool register_loader_observer() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    g_register_loader_notification =
        module_procedure<RegisterLoaderNotification>(
            ntdll, "LdrRegisterDllNotification");
    g_unregister_loader_notification =
        module_procedure<UnregisterLoaderNotification>(
            ntdll, "LdrUnregisterDllNotification");
    if (!g_register_loader_notification ||
        !g_unregister_loader_notification) {
        return false;
    }
    return g_register_loader_notification(
               0, loader_notification, nullptr,
               &g_loader_notification_cookie) >= 0;
}

void unregister_loader_observer() {
    if (g_loader_notification_cookie && g_unregister_loader_notification) {
        g_unregister_loader_notification(g_loader_notification_cookie);
        g_loader_notification_cookie = nullptr;
    }
}

bool preload_original_wrapper() {
    const std::filesystem::path path =
        std::filesystem::path(parent_path(module_path(nullptr))) /
        L"dancearound_data" / L"plugins" / L"x86_64" /
        L"visionposewrapper.dll";
    g_preloaded_wrapper = LoadLibraryExW(
        path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_preloaded_wrapper) {
        const DWORD error = GetLastError();
        report_error(
            "could not preload original visionposewrapper.dll (Win32 " +
            std::to_string(error) + ")");
        return false;
    }
    return true;
}

enum class NvidiaDirectory {
    cuda,
    cudnn,
    tensorrt,
};

struct NvidiaLibrary {
    NvidiaDirectory directory;
    const wchar_t* name;
    std::array<const char*, 2> required_exports;
};

bool preload_nvidia_runtime() {
    if (g_config.nvidia_runtime_root.empty() && g_config.cuda_root.empty()) {
        return true;
    }
    if (g_config.nvidia_runtime_root.empty() || g_config.cuda_root.empty()) {
        report_error(
            "NvidiaRuntimeRoot and CudaRoot must either both be set or both "
            "be empty");
        return false;
    }

    static constexpr std::array<NvidiaLibrary, 14> libraries{{
        {NvidiaDirectory::cuda, L"cudart64_110.dll", {}},
        {NvidiaDirectory::cuda, L"cublasLt64_11.dll", {}},
        {NvidiaDirectory::cuda, L"cublas64_11.dll", {}},
        {NvidiaDirectory::cuda, L"nvrtc-builtins64_111.dll", {}},
        {NvidiaDirectory::cuda, L"nvrtc64_111_0.dll", {}},
        {NvidiaDirectory::cudnn, L"cudnn_ops_infer64_8.dll", {}},
        {NvidiaDirectory::cudnn, L"cudnn_cnn_infer64_8.dll", {}},
        {NvidiaDirectory::cudnn, L"cudnn_adv_infer64_8.dll", {}},
        {NvidiaDirectory::cudnn, L"cudnn64_8.dll", {"cudnnGetVersion"}},
        {NvidiaDirectory::tensorrt, L"myelin64_1.dll", {}},
        {NvidiaDirectory::tensorrt, L"nvinfer.dll",
         {"createInferBuilder_INTERNAL", "createInferRuntime_INTERNAL"}},
        {NvidiaDirectory::tensorrt, L"nvinfer_plugin.dll",
         {"initLibNvInferPlugins"}},
        {NvidiaDirectory::tensorrt, L"nvonnxparser.dll", {}},
        {NvidiaDirectory::tensorrt, L"nvparsers.dll", {}},
    }};

    const std::filesystem::path cuda_directory =
        g_config.cuda_root / L"bin";
    const std::filesystem::path cudnn_directory =
        g_config.nvidia_runtime_root / L"cudnn-8.0.4.30" / L"bin";
    const std::filesystem::path tensorrt_directory =
        g_config.nvidia_runtime_root / L"TensorRT-7.2.1.6" / L"lib";

    for (const NvidiaLibrary& library : libraries) {
        const std::filesystem::path* directory = &cuda_directory;
        if (library.directory == NvidiaDirectory::cudnn) {
            directory = &cudnn_directory;
        } else if (library.directory == NvidiaDirectory::tensorrt) {
            directory = &tensorrt_directory;
        }
        const std::filesystem::path path = *directory / library.name;
        HMODULE module = LoadLibraryExW(
            path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            report_error(
                "could not preload NVIDIA runtime library " +
                path.filename().u8string() + " (Win32 " +
                std::to_string(GetLastError()) + ")");
            return false;
        }
        g_nvidia_runtime_modules.push_back(module);
        for (const char* name : library.required_exports) {
            if (name && !GetProcAddress(module, name)) {
                report_error(
                    path.filename().u8string() +
                    " is not the required NVIDIA runtime version");
                return false;
            }
        }
    }
    return true;
}

void __cdecl destroy_callback() {
    unregister_loader_observer();
    restore_vp4u_export_redirect();
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_frame_buffers.clear();
    }
    if (g_preloaded_wrapper) {
        FreeLibrary(g_preloaded_wrapper);
        g_preloaded_wrapper = nullptr;
    }
    g_original_wrapper = nullptr;
    g_original_get_version = nullptr;
    g_original_preboot = nullptr;
    g_original_shutdown = nullptr;
    g_original_get_api_table = nullptr;
    for (auto module = g_nvidia_runtime_modules.rbegin();
         module != g_nvidia_runtime_modules.rend(); ++module) {
        FreeLibrary(*module);
    }
    g_nvidia_runtime_modules.clear();
    log_message(ANYGEAR_SPICE_LOG_INFO, "native D4xx bridge shutdown");
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl spice_sdk_entry_point(
    anygear_spice_init_func init) {
    if (!init) return 0;
    g_spice = {};
    g_spice.size = sizeof(g_spice);
    if (init(0, destroy_callback, &g_spice) != ANYGEAR_SPICE_SUCCESS) {
        return 0;
    }
    log_message(ANYGEAR_SPICE_LOG_INFO,
        std::string("stage 1/6: Anygear ") + ANYGEAR_VERSION +
        " native D4xx bridge loaded; original VisionPose retained");
    ensure_config();
    if (!preload_nvidia_runtime()) return 0;
    log_message(ANYGEAR_SPICE_LOG_INFO,
        g_nvidia_runtime_modules.empty()
            ? "stage 2/6: using the game or system NVIDIA runtime"
            : "stage 2/6: configured NVIDIA runtime preloaded (14 modules)");
    WrapperState state = g_wrapper_state.load(std::memory_order_acquire);
    if (state == WrapperState::waiting) {
        if (!preload_original_wrapper()) return 0;
        state = g_wrapper_state.load(std::memory_order_acquire);
    }
    if (state == WrapperState::incompatible) {
        report_error(
            "visionposewrapper.dll is not the original librealsense-based "
            "VP4U wrapper");
        return 0;
    }
    if (state == WrapperState::waiting) {
        report_error(
            "original visionposewrapper.dll was not observed before Unity "
            "pose startup");
        return 0;
    }
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "stage 3/6: original VP4U observed before Unity pose startup");
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "stage 4/6: D435 COLOR/depth contract adapted to D4xx IR/depth");
    if (!register_wrapper_aliases()) return 0;
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "stage 5/6: VP4U forwarding active with realtime-only API table");
    log_message(ANYGEAR_SPICE_LOG_INFO,
        "stage 6/6: native D4xx bridge ready; original VisionPose is active");
    return 1;
}

extern "C" __declspec(dllexport) const char* __cdecl
danceAroundAnygearGetBuildInfo() {
    static const std::string information =
        std::string("dance-around-anygear/") + ANYGEAR_VERSION +
        ";mode=native-d4xx-bridge;vp4u=original;activation=loader-notify;"
        "camera=ir-y8-bgr24;analysis=realtime";
    return information.c_str();
}

namespace vp4u {

extern "C" __declspec(dllexport) int __cdecl vp4uGetVersion() {
    return g_original_get_version ? g_original_get_version() : 0;
}

extern "C" __declspec(dllexport) bool __cdecl vp4uPreboot(
    const char* preboot_config_json,
    OnLogCallback logger_callback,
    void* logger_context) {
    return g_original_preboot && g_original_preboot(
        preboot_config_json, logger_callback, logger_context);
}

extern "C" __declspec(dllexport) void __cdecl vp4uShutdown() {
    if (g_original_shutdown) g_original_shutdown();
}

extern "C" __declspec(dllexport) bool __cdecl vp4uGetApiTable(
    int api_version,
    FunctionPointerTable* table,
    OnLogCallback logger_callback,
    void* logger_context) {
    if (!g_original_get_api_table ||
        !g_original_get_api_table(
            api_version, table, logger_callback, logger_context)) {
        return false;
    }
    adapt_vp4u_api_table(table);
    return true;
}

} // namespace vp4u

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        if (!register_loader_observer()) return FALSE;
    }
    return TRUE;
}
