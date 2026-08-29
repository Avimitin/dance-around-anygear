// webcam.cpp - Media Foundation USB webcam capture.
#include "webcam.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace vp4u {

static bool shim_log_enabled() {
    static int v = []() {
        const char* e = std::getenv("VP4U_LOG");
        return (e && e[0] && e[0] != '0') ? 1 : 0;
    }();
    return v != 0;
}
#define SHIM_LOG(...) do { if (shim_log_enabled()) { std::fprintf(stderr, "[vp4u] " __VA_ARGS__); std::fflush(stderr); } } while (0)

template <class T> static void safe_release(T** pp) {
    if (*pp) { (*pp)->Release(); *pp = nullptr; }
}

struct CameraSource::Impl {
    int index = -1;                    // requested physical camera index
    std::atomic<bool> stop{false};
    std::thread worker;
    std::atomic<bool> real{false};     // a real camera is currently open
    std::atomic<bool> haveFrame{false};
    std::mutex mtx;
    std::vector<uint8_t> frame;        // latest frame, native size, BGR8 top-down
    std::atomic<int> width{0}, height{0}; // native size

    explicit Impl(int idx) : index(idx) {}

    void start() {
        worker = std::thread([this] {
            const HRESULT com_status =
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            run();
            if (SUCCEEDED(com_status)) CoUninitialize();
        });
    }

    void join() {
        stop = true;
        if (worker.joinable()) worker.join();
    }

    void run() {
        while (!stop) {
            if (!capture_once()) {
                real = false;
                if (stop) break;
                SHIM_LOG("cam %d: open/capture failed, retry in 2s\n", index);
                for (int i = 0; i < 20 && !stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    // Open the camera, pump samples until failure/stop. Returns false on any failure.
    bool capture_once() {
        real = false;
        haveFrame = false;
        IMFAttributes* attrs = nullptr;
        IMFAttributes* readerAttrs = nullptr;
        IMFActivate** devices = nullptr;
        IMFMediaSource* source = nullptr;
        IMFSourceReader* reader = nullptr;
        IMFMediaType* outType = nullptr;
        IMFMediaType* curType = nullptr;
        UINT32 count = 0;

        HRESULT hr = MFCreateAttributes(&attrs, 1);
        if (FAILED(hr)) goto done;
        hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(hr)) goto done;
        hr = MFEnumDeviceSources(attrs, &devices, &count);
        if (FAILED(hr) || count == 0) goto done;
        if (index < 0 || (UINT32)index >= count) goto done;

        hr = devices[index]->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr)) goto done;
        hr = MFCreateAttributes(&readerAttrs, 1);
        if (FAILED(hr)) goto done;
        hr = readerAttrs->SetUINT32(
            MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (FAILED(hr)) goto done;
        hr = MFCreateSourceReaderFromMediaSource(
            source, readerAttrs, &reader);
        if (FAILED(hr)) goto done;

        // Ask the source reader for RGB32 (it inserts color converters as needed).
        hr = MFCreateMediaType(&outType);
        if (FAILED(hr)) goto done;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outType);
        if (FAILED(hr)) goto done;

        hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &curType);
        if (FAILED(hr)) goto done;
        {
            UINT32 w = 0, h = 0;
            if (FAILED(MFGetAttributeSize(curType, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
                goto done;
            LONG stride = (LONG)w * 4; // MF_MT_DEFAULT_STRIDE: positive => bottom-up rows
            UINT32 strideAttr = 0;
            if (SUCCEEDED(curType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr)))
                stride = (LONG)(int32_t)strideAttr;
            {
                std::lock_guard<std::mutex> lk(mtx);
                width = (int)w; height = (int)h;
                frame.assign((size_t)w * h * 3, 0);
            }
            real = true;
            SHIM_LOG("cam %d: opened, native %ux%u stride %ld\n", index, w, h, stride);

            while (!stop) {
                DWORD streamIndex = 0, flags = 0;
                LONGLONG ts = 0;
                IMFSample* sample = nullptr;
                hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                        &streamIndex, &flags, &ts, &sample);
                if (FAILED(hr)) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { safe_release(&sample); break; }
                if (sample) {
                    IMFMediaBuffer* buf = nullptr;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                        BYTE* data = nullptr;
                        DWORD curLen = 0;
                        if (SUCCEEDED(buf->Lock(&data, nullptr, &curLen)) && data) {
                            std::lock_guard<std::mutex> lk(mtx);
                            const LONG absStride = stride < 0 ? -stride : stride;
                            const bool bottomUp = stride > 0;
                            if ((DWORD)((size_t)absStride * h) <= curLen) {
                                for (UINT32 y = 0; y < h; y++) {
                                    const BYTE* src = data + (size_t)(bottomUp ? (h - 1 - y) : y) * absStride;
                                    uint8_t* dst = frame.data() + (size_t)y * w * 3;
                                    for (UINT32 x = 0; x < w; x++) {
                                        dst[x * 3 + 0] = src[x * 4 + 0]; // B
                                        dst[x * 3 + 1] = src[x * 4 + 1]; // G
                                        dst[x * 3 + 2] = src[x * 4 + 2]; // R
                                    }
                                }
                                haveFrame = true;
                            }
                            buf->Unlock();
                        }
                        buf->Release();
                    }
                    safe_release(&sample);
                }
            }
        }

    done:
        safe_release(&curType);
        safe_release(&outType);
        safe_release(&reader);
        safe_release(&readerAttrs);
        if (source) { source->Shutdown(); safe_release(&source); }
        if (devices) {
            for (UINT32 i = 0; i < count; i++) safe_release(&devices[i]);
            CoTaskMemFree(devices);
        }
        safe_release(&attrs);
        // Any exit means capture stopped. The outer loop applies a bounded
        // reconnect delay unless shutdown was requested.
        return false;
    }
};

// ---- registry: one shared CameraSource per physical index ----
static std::mutex g_regMtx;
static std::map<int, std::weak_ptr<CameraSource>> g_registry;
static std::once_flag g_mfOnce;
static bool g_mfStarted = false;

static void mf_global_init() {
    std::call_once(g_mfOnce, [] {
        HRESULT hr = MFStartup(MF_VERSION);
        g_mfStarted = SUCCEEDED(hr);
        SHIM_LOG("MFStartup: %s\n", g_mfStarted ? "ok" : "FAILED");
    });
}

std::shared_ptr<CameraSource> CameraSource::acquire(int index) {
    mf_global_init();
    std::lock_guard<std::mutex> lk(g_regMtx);
    auto it = g_registry.find(index);
    if (it != g_registry.end()) {
        if (auto sp = it->second.lock()) return sp;
    }
    auto sp = std::shared_ptr<CameraSource>(new CameraSource(index));
    if (index >= 0 && g_mfStarted) sp->impl_->start();
    g_registry[index] = sp;
    return sp;
}

CameraSource::CameraSource(int index) : impl_(new Impl(index)) {}
CameraSource::~CameraSource() {
    impl_->join();
    delete impl_;
}

bool CameraSource::is_real_camera() const {
    return impl_->real && impl_->haveFrame;
}
int CameraSource::native_width() const { return impl_->width; }
int CameraSource::native_height() const { return impl_->height; }

bool CameraSource::wait_until_ready(int timeout_ms) const {
    if (impl_->index < 0) return true;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (is_real_camera()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return is_real_camera();
}

void scale_bgr8_bilinear(const uint8_t* src, int sw, int sh,
                         uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        double sy = (y + 0.5) * sh / dh - 0.5;
        int y0 = (int)std::floor(sy);
        double fy = sy - y0;
        if (y0 < 0) { y0 = 0; fy = 0; }
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        for (int x = 0; x < dw; x++) {
            double sx = (x + 0.5) * sw / dw - 0.5;
            int x0 = (int)std::floor(sx);
            double fx = sx - x0;
            if (x0 < 0) { x0 = 0; fx = 0; }
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            const uint8_t* p00 = src + ((size_t)y0 * sw + x0) * 3;
            const uint8_t* p01 = src + ((size_t)y0 * sw + x1) * 3;
            const uint8_t* p10 = src + ((size_t)y1 * sw + x0) * 3;
            const uint8_t* p11 = src + ((size_t)y1 * sw + x1) * 3;
            uint8_t* q = dst + ((size_t)y * dw + x) * 3;
            for (int c = 0; c < 3; c++) {
                double v = p00[c] * (1 - fx) * (1 - fy) + p01[c] * fx * (1 - fy)
                         + p10[c] * (1 - fx) * fy + p11[c] * fx * fy;
                q[c] = (uint8_t)(v + 0.5);
            }
        }
    }
}

bool CameraSource::get_frame_bgr8(std::vector<uint8_t>& out, int w, int h) {
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->real && impl_->haveFrame &&
            impl_->width > 0 && impl_->height > 0) {
            out.resize((size_t)w * h * 3);
            if (impl_->width == w && impl_->height == h)
                out = impl_->frame;
            else
                scale_bgr8_bilinear(impl_->frame.data(), impl_->width, impl_->height,
                                    out.data(), w, h);
            return true;
        }
    }
    if (impl_->index >= 0) {
        out.clear();
        return false;
    }
    double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    make_synthetic_bgr8(out, w, h, t, impl_->index);
    return false;
}

void make_synthetic_bgr8(std::vector<uint8_t>& out, int w, int h, double t, int tag) {
    out.resize((size_t)w * h * 3);
    const int barPos = (int)std::fmod(t * 120.0, (double)w + 160.0) - 80;
    const int tintB = (tag & 1) ? 96 : 32;
    const int tintR = (tag & 1) ? 32 : 96;
    const int w1 = w > 1 ? w - 1 : 1, h1 = h > 1 ? h - 1 : 1;
    for (int y = 0; y < h; y++) {
        const int gy = (y * 255) / h1;
        uint8_t* row = out.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            int v = (x * 255) / w1;
            int d = x - barPos; if (d < 0) d = -d;
            if (d < 40) v = v / 2 + (127 * (40 - d)) / 40 + 64;
            if (v > 255) v = 255;
            uint8_t* p = row + (size_t)x * 3;
            p[0] = (uint8_t)((v + tintB) / 2);        // B
            p[1] = (uint8_t)((gy + v) / 2);           // G
            p[2] = (uint8_t)((255 - v + tintR) / 2);  // R
        }
    }
}

} // namespace vp4u

namespace vp4u {

int count_physical_cameras() {
    const HRESULT com_status =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) return 0;
    const bool uninitialize_com = SUCCEEDED(com_status);
    mf_global_init();
    if (!g_mfStarted) {
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    IMFAttributes* attrs = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFCreateAttributes(&attrs, 1))) {
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    safe_release(&attrs);
    if (FAILED(hr)) {
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    if (devices) {
        for (UINT32 i = 0; i < count; i++) safe_release(&devices[i]);
        CoTaskMemFree(devices);
    }
    if (uninitialize_com) CoUninitialize();
    return (int)count;
}

} // namespace vp4u
