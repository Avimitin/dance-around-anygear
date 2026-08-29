#include "vp4u/pose.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <wincodec.h>

namespace {

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::wstring widen(const char* value) {
    if (!value) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), count);
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

bool decode_bgr(const std::wstring& path, std::vector<std::uint8_t>* pixels,
                int* width, int* height) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;

    HRESULT status = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(status)) goto done;
    status = factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
        &decoder);
    if (FAILED(status)) goto done;
    status = decoder->GetFrame(0, &frame);
    if (FAILED(status)) goto done;
    status = factory->CreateFormatConverter(&converter);
    if (FAILED(status)) goto done;
    status = converter->Initialize(
        frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(status)) goto done;
    {
        UINT decoded_width = 0;
        UINT decoded_height = 0;
        status = converter->GetSize(&decoded_width, &decoded_height);
        if (FAILED(status) || decoded_width == 0 || decoded_height == 0) {
            goto done;
        }
        const UINT stride = decoded_width * 3;
        pixels->resize(static_cast<std::size_t>(stride) * decoded_height);
        status = converter->CopyPixels(
            nullptr, stride, static_cast<UINT>(pixels->size()), pixels->data());
        if (FAILED(status)) goto done;
        *width = static_cast<int>(decoded_width);
        *height = static_cast<int>(decoded_height);
        ok = true;
    }

done:
    release(converter);
    release(frame);
    release(decoder);
    release(factory);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr,
            "usage: anygear_mediapipe_probe <runtime-dir> <model.task> <image> [iterations]\n");
        return 2;
    }
    const int iterations = argc == 5 ? std::atoi(argv[4]) : 60;
    if (iterations < 1 || iterations > 600) return 2;

    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status)) {
        std::fprintf(stderr, "FAIL: CoInitializeEx 0x%08lx\n",
                     static_cast<unsigned long>(com_status));
        return 3;
    }

    std::vector<std::uint8_t> image;
    int width = 0;
    int height = 0;
    if (!decode_bgr(widen(argv[3]), &image, &width, &height)) {
        std::fprintf(stderr, "FAIL: could not decode %s\n", argv[3]);
        CoUninitialize();
        return 4;
    }

    vp4u::PoseEngine engine;
    vp4u::PoseCalib calibration;
    std::string error;
    if (!engine.init(widen(argv[1]), argv[2], calibration, &error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        CoUninitialize();
        return 5;
    }

    vp4u::PoseResult result;
    int valid_results = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (engine.infer(image.data(), width, height, &result) && result.valid) {
            ++valid_results;
        }
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    engine.shutdown();
    CoUninitialize();

    if (!result.valid || valid_results < iterations * 9 / 10) {
        std::fprintf(stderr,
            "FAIL: MediaPipe valid poses %d/%d (last confidence=%.3f)\n",
            valid_results, iterations, result.confidence);
        return 6;
    }
    for (int landmark = 0; landmark < vp4u::kMpLandmarkCount; ++landmark) {
        for (int component = 0; component < 3; ++component) {
            if (!std::isfinite(result.world[landmark][component])) {
                std::fprintf(stderr, "FAIL: non-finite world landmark %d\n",
                             landmark);
                return 7;
            }
        }
    }

    const float hip_x =
        (result.world[vp4u::MP_LeftHip][0] +
         result.world[vp4u::MP_RightHip][0]) * 0.5f;
    const float hip_y =
        (result.world[vp4u::MP_LeftHip][1] +
         result.world[vp4u::MP_RightHip][1]) * 0.5f;
    const float hip_z =
        (result.world[vp4u::MP_LeftHip][2] +
         result.world[vp4u::MP_RightHip][2]) * 0.5f;
    std::printf(
        "PASS: MediaPipe pose %dx%d valid=%d/%d rate=%.1f FPS "
        "confidence=%.3f hip=(%.3f, %.3f, %.3f)\n",
        width, height, valid_results, iterations,
        elapsed > 0.0 ? iterations / elapsed : 0.0,
        result.confidence, hip_x, hip_y, hip_z);
    return 0;
}
