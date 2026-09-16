#include "webcam.h"
#include "vcam_shm.h"
#include "omt.h"
#include "settings.h"

#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
WebcamOutput g_webcam;

// Bilinear scale of a top-down BGRA image. Only used when the source and the
// configured webcam size differ; a matching size takes the memcpy path.
void scale_bgra(const uint8_t* src, int sw, int sh, int src_stride,
                uint8_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    const int dst_stride = dw * 4;
    const uint32_t x_ratio = static_cast<uint32_t>((sw << 16) / dw);
    const uint32_t y_ratio = static_cast<uint32_t>((sh << 16) / dh);

    for (int y = 0; y < dh; ++y) {
        const uint32_t sy = (static_cast<uint32_t>(y) * y_ratio) >> 16;
        const uint32_t fy = ((static_cast<uint32_t>(y) * y_ratio) & 0xFFFF) >> 8;
        const uint32_t sy1 = std::min<uint32_t>(sy + 1, static_cast<uint32_t>(sh - 1));

        const uint8_t* row0 = src + static_cast<size_t>(sy)  * src_stride;
        const uint8_t* row1 = src + static_cast<size_t>(sy1) * src_stride;
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;

        for (int x = 0; x < dw; ++x) {
            const uint32_t sx = (static_cast<uint32_t>(x) * x_ratio) >> 16;
            const uint32_t fx = ((static_cast<uint32_t>(x) * x_ratio) & 0xFFFF) >> 8;
            const uint32_t sx1 = std::min<uint32_t>(sx + 1, static_cast<uint32_t>(sw - 1));

            for (int c = 0; c < 4; ++c) {
                const uint32_t p00 = row0[sx * 4 + c],  p01 = row0[sx1 * 4 + c];
                const uint32_t p10 = row1[sx * 4 + c],  p11 = row1[sx1 * 4 + c];
                const uint32_t top = p00 + (((p01 - p00) * fx) >> 8);
                const uint32_t bot = p10 + (((p11 - p10) * fx) >> 8);
                out[x * 4 + c] = static_cast<uint8_t>(top + (((bot - top) * fy) >> 8));
            }
        }
    }
}

// Owns the shared mapping the virtual camera filter reads from.
class SharedFrames {
public:
    bool open(int width, int height, int fps) {
        close();
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      0, VCAM_TOTAL_BYTES, VCAM_SHM_NAME);
        if (!mapping_) {
            util::logf("webcam: CreateFileMapping failed err=%lu", GetLastError());
            return false;
        }
        base_ = static_cast<uint8_t*>(
            MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, VCAM_TOTAL_BYTES));
        if (!base_) {
            util::logf("webcam: MapViewOfFile failed err=%lu", GetLastError());
            close();
            return false;
        }

        header_ = reinterpret_cast<VCamHeader*>(base_);
        std::memset(header_, 0, sizeof(VCamHeader));
        header_->magic        = VCAM_MAGIC;
        header_->version      = VCAM_VERSION;
        header_->width        = static_cast<uint32_t>(width);
        header_->height       = static_cast<uint32_t>(height);
        header_->fps_num      = static_cast<uint32_t>(fps);
        header_->fps_den      = 1;
        header_->buffer_count = VCAM_BUFFERS;
        header_->buffer_bytes = static_cast<uint32_t>(width * height * 4);
        header_->producer_pid = GetCurrentProcessId();
        header_->write_index  = 0;
        header_->sequence     = 0;
        header_->heartbeat_ms = GetTickCount64();
        MemoryBarrier();
        header_->active       = 1;

        width_ = width;
        height_ = height;
        return true;
    }

    void close() {
        if (header_) {
            header_->active = 0;
            MemoryBarrier();
        }
        if (base_)    { UnmapViewOfFile(base_); base_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        header_ = nullptr;
    }

    uint8_t* next_buffer() {
        if (!header_) return nullptr;
        const uint32_t idx = (header_->write_index + 1) % VCAM_BUFFERS;
        return base_ + VCAM_HEADER_BYTES + static_cast<size_t>(idx) * VCAM_FRAME_BYTES;
    }

    void commit() {
        if (!header_) return;
        const uint32_t idx = (header_->write_index + 1) % VCAM_BUFFERS;
        // Publish the pixels before the index that points at them.
        MemoryBarrier();
        header_->write_index  = idx;
        header_->sequence     = header_->sequence + 1;
        header_->heartbeat_ms = GetTickCount64();
        MemoryBarrier();
    }

    void heartbeat() {
        if (header_) header_->heartbeat_ms = GetTickCount64();
    }

    bool valid() const { return header_ != nullptr; }
    int  width() const { return width_; }
    int  height() const { return height_; }

private:
    HANDLE      mapping_ = nullptr;
    uint8_t*    base_ = nullptr;
    VCamHeader* header_ = nullptr;
    int         width_ = 0, height_ = 0;
};

} // namespace

WebcamOutput& webcam() { return g_webcam; }

std::wstring WebcamOutput::filter_path() {
    return util::exe_dir() + L"\\omtmini_vcam.dll";
}

bool WebcamOutput::filter_registered() {
    // The filter registers its CLSID under the per-user class root.
    HKEY key = nullptr;
    const wchar_t* path =
        L"Software\\Classes\\CLSID\\{6B7A9E54-2C1D-4F8B-9E3A-7D5C2F1A8B40}\\InprocServer32";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

bool WebcamOutput::register_filter(bool enable, std::wstring* message) {
    const std::wstring dll = filter_path();

    DWORD attrs = GetFileAttributesW(dll.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (message)
            *message = L"omtmini_vcam.dll was not found next to OMTMini.exe.";
        return false;
    }

    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) {
        if (message) *message = L"The virtual camera DLL could not be loaded.";
        util::logf("webcam: LoadLibrary vcam failed err=%lu", GetLastError());
        return false;
    }

    using RegFn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<RegFn>(reinterpret_cast<void*>(
        GetProcAddress(mod, enable ? "DllRegisterServer" : "DllUnregisterServer")));
    if (!fn) {
        FreeLibrary(mod);
        if (message) *message = L"The virtual camera DLL is missing its registration entry point.";
        return false;
    }

    const HRESULT hr = fn();
    FreeLibrary(mod);

    if (FAILED(hr)) {
        if (message) *message = L"Registration failed. See omtmini.log for details.";
        util::logf("webcam: %s returned 0x%08lx",
                   enable ? "DllRegisterServer" : "DllUnregisterServer",
                   static_cast<unsigned long>(hr));
        return false;
    }

    if (message)
        *message = enable ? L"Virtual camera registered for this user."
                          : L"Virtual camera removed.";
    util::logf("webcam: filter %s", enable ? "registered" : "unregistered");
    return true;
}

bool WebcamOutput::start(const std::string& source) {
    if (running_) stop();
    if (source.empty() || !omt::loaded()) return false;

    running_ = true;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = WebcamStats{};
        stats_.running = true;
        stats_.source  = source;
    }
    thread_ = std::thread([this, source] { run(source); });
    return true;
}

void WebcamOutput::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = WebcamStats{};
    util::logf("webcam: stopped");
}

WebcamStats WebcamOutput::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void WebcamOutput::run(std::string source) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Copied so the settings window cannot mutate these mid stream.
    const Settings cfg = settings();
    const int out_w = std::clamp(cfg.webcam_width,  160, VCAM_MAX_WIDTH);
    const int out_h = std::clamp(cfg.webcam_height, 120, VCAM_MAX_HEIGHT);
    const int fps   = std::clamp(cfg.webcam_fps, 1, 120);

    SharedFrames shm;
    if (!shm.open(out_w, out_h, fps)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.error = "Could not create the shared frame buffer.";
        stats_.running = false;
        running_ = false;
        CoUninitialize();
        return;
    }

    // BGRA out of libomt keeps this thread to a scale-and-copy.
    omt::Receiver receiver;
    if (!receiver.open(source, OMTFrameType_Video, OMTPreferredVideoFormat_BGRA,
                       OMTReceiveFlags_None)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.error = "Could not connect to the source.";
        stats_.running = false;
        running_ = false;
        shm.close();
        CoUninitialize();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.width  = out_w;
        stats_.height = out_h;
    }
    util::logf("webcam: '%s' -> %dx%d @%d", source.c_str(), out_w, out_h, fps);

    int64_t frames = 0;
    int64_t window_start = util::now_ms();
    int     window_frames = 0;
    int64_t last_frame_ms = 0;

    while (running_) {
        OMTMediaFrame* f = receiver.receive(OMTFrameType_Video, 100);
        if (!f) {
            // Keep the heartbeat fresh so the filter shows "waiting" rather
            // than dropping the device entirely.
            shm.heartbeat();
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.connected = (util::now_ms() - last_frame_ms) < 1000;
            continue;
        }
        if (f->Type != OMTFrameType_Video || !f->Data || f->Width <= 0 || f->Height <= 0)
            continue;

        uint8_t* dst = shm.next_buffer();
        if (!dst) break;

        const int src_stride = f->Stride > 0 ? f->Stride : f->Width * 4;

        if (f->Codec == OMTCodec_BGRA) {
            if (f->Width == out_w && f->Height == out_h) {
                const int row = out_w * 4;
                for (int y = 0; y < out_h; ++y)
                    std::memcpy(dst + static_cast<size_t>(y) * row,
                                static_cast<const uint8_t*>(f->Data) +
                                    static_cast<size_t>(y) * src_stride,
                                static_cast<size_t>(row));
            } else {
                scale_bgra(static_cast<const uint8_t*>(f->Data), f->Width, f->Height,
                           src_stride, dst, out_w, out_h);
            }
            shm.commit();
            ++frames;
            ++window_frames;
            last_frame_ms = util::now_ms();
        }

        const int64_t elapsed = util::now_ms() - window_start;
        if (elapsed >= 1000) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.fps       = static_cast<float>(window_frames) * 1000.0f / elapsed;
            stats_.frames    = frames;
            stats_.connected = true;
            window_frames = 0;
            window_start  = util::now_ms();
        }
    }

    receiver.close();
    shm.close();
    CoUninitialize();
}
