#include "capture.h"
#include "omt.h"
#include "settings.h"
#include "netinfo.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cstring>
#include <cmath>

using util::ComPtr;

namespace {
DesktopCapture g_capture;

// Blends a captured pointer shape into the BGRA frame buffer.
void composite_pointer(uint8_t* frame, int fw, int fh, int stride,
                       const std::vector<uint8_t>& shape,
                       const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                       int px, int py) {
    if (shape.empty() || info.Width == 0 || info.Height == 0) return;

    const int sw = static_cast<int>(info.Width);
    const int pitch = static_cast<int>(info.Pitch);

    if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        const int sh = static_cast<int>(info.Height);
        for (int y = 0; y < sh; ++y) {
            const int dy = py + y;
            if (dy < 0 || dy >= fh) continue;
            const auto* src = shape.data() + static_cast<size_t>(y) * pitch;
            auto* dst = frame + static_cast<size_t>(dy) * stride;
            for (int x = 0; x < sw; ++x) {
                const int dx = px + x;
                if (dx < 0 || dx >= fw) continue;
                const uint8_t a = src[x * 4 + 3];
                if (a == 0) continue;
                uint8_t* d = dst + dx * 4;
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<uint8_t>((src[x * 4 + c] * a + d[c] * (255 - a)) / 255);
            }
        }
    } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Top half is the AND mask, bottom half the XOR mask.
        const int sh = static_cast<int>(info.Height) / 2;
        for (int y = 0; y < sh; ++y) {
            const int dy = py + y;
            if (dy < 0 || dy >= fh) continue;
            const auto* and_row = shape.data() + static_cast<size_t>(y) * pitch;
            const auto* xor_row = shape.data() + static_cast<size_t>(y + sh) * pitch;
            auto* dst = frame + static_cast<size_t>(dy) * stride;
            for (int x = 0; x < sw; ++x) {
                const int dx = px + x;
                if (dx < 0 || dx >= fw) continue;
                const uint8_t bit  = static_cast<uint8_t>(0x80 >> (x & 7));
                const bool and_bit = (and_row[x / 8] & bit) != 0;
                const bool xor_bit = (xor_row[x / 8] & bit) != 0;
                uint8_t* d = dst + dx * 4;
                for (int c = 0; c < 3; ++c) {
                    if (!and_bit) d[c] = xor_bit ? 0xFF : 0x00;
                    else if (xor_bit) d[c] = static_cast<uint8_t>(d[c] ^ 0xFF);
                }
            }
        }
    } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        const int sh = static_cast<int>(info.Height);
        for (int y = 0; y < sh; ++y) {
            const int dy = py + y;
            if (dy < 0 || dy >= fh) continue;
            const auto* src = shape.data() + static_cast<size_t>(y) * pitch;
            auto* dst = frame + static_cast<size_t>(dy) * stride;
            for (int x = 0; x < sw; ++x) {
                const int dx = px + x;
                if (dx < 0 || dx >= fw) continue;
                uint8_t* d = dst + dx * 4;
                // Alpha byte selects replace (0) or XOR (0xFF).
                if (src[x * 4 + 3] == 0) {
                    for (int c = 0; c < 3; ++c) d[c] = src[x * 4 + c];
                } else {
                    for (int c = 0; c < 3; ++c)
                        d[c] = static_cast<uint8_t>(d[c] ^ src[x * 4 + c]);
                }
            }
        }
    }
}
} // namespace

DesktopCapture& desktop_capture() { return g_capture; }

std::vector<CaptureMonitor> DesktopCapture::enumerate_monitors() {
    std::vector<CaptureMonitor> out;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())))
        return out;

    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.put()) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;

            CaptureMonitor m;
            m.device  = desc.DeviceName;
            m.width   = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            m.height  = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            m.adapter = static_cast<int>(a);
            m.output  = static_cast<int>(o);
            m.primary = desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;

            wchar_t label[160];
            swprintf(label, 160, L"%s  (%dx%d)%s", desc.DeviceName, m.width, m.height,
                     m.primary ? L"  primary" : L"");
            m.name = label;
            out.push_back(std::move(m));
        }
    }
    return out;
}

bool DesktopCapture::start() {
    if (running_) return true;
    if (!omt::loaded()) return false;

    running_ = true;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = CaptureStats{};
        stats_.running = true;
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void DesktopCapture::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    audio_.stop();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = CaptureStats{};
    util::logf("capture: stopped");
}

CaptureStats DesktopCapture::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void DesktopCapture::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto fail = [this](const char* why) {
        util::logf("capture: %s", why);
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.error   = why;
        stats_.running = false;
    };

    // Copied so the settings window cannot mutate these mid capture.
    const Settings cfg = settings();

    // ---- pick the output ----
    const auto monitors = enumerate_monitors();
    if (monitors.empty()) { fail("no monitors found"); running_ = false; CoUninitialize(); return; }

    const int index = std::clamp(cfg.capture_monitor, 0, static_cast<int>(monitors.size()) - 1);
    const CaptureMonitor& mon = monitors[index];

    // ---- dedicated device on the adapter owning that output ----
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
        fail("CreateDXGIFactory1 failed"); running_ = false; CoUninitialize(); return;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(mon.adapter), adapter.put()))) {
        fail("adapter not found"); running_ = false; CoUninitialize(); return;
    }
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(static_cast<UINT>(mon.output), output.put()))) {
        fail("output not found"); running_ = false; CoUninitialize(); return;
    }
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), output1.put_void()))) {
        fail("Desktop Duplication needs Windows 8 or later");
        running_ = false; CoUninitialize(); return;
    }

    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                 device.put(), &got, context.put()))) {
        fail("D3D11 device creation failed"); running_ = false; CoUninitialize(); return;
    }

    ComPtr<IDXGIOutputDuplication> dupl;
    HRESULT hr = output1->DuplicateOutput(device.get(), dupl.put());
    if (FAILED(hr)) {
        // A full screen exclusive app or another duplication client blocks this.
        fail(hr == DXGI_ERROR_UNSUPPORTED
                 ? "Desktop Duplication is unavailable on this adapter"
                 : "DuplicateOutput failed (another capture app may hold it)");
        running_ = false; CoUninitialize(); return;
    }

    const int width  = mon.width;
    const int height = mon.height;

    // ---- staging texture for CPU readback ----
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = static_cast<UINT>(width);
    sd.Height = static_cast<UINT>(height);
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, staging.put()))) {
        fail("staging texture creation failed"); running_ = false; CoUninitialize(); return;
    }

    // ---- sender ----
    // Noted before the sender exists, so the port it takes can be identified
    // rather than guessed at.
    const auto ports_before = netinfo::listening_ports(cfg.port_start, cfg.port_end);

    omt::Sender sender;
    const std::string name = cfg.capture_name.empty() ? std::string("Desktop") : cfg.capture_name;
    if (!sender.open(name, static_cast<OMTQuality>(cfg.capture_quality))) {
        fail("omt_send_create failed"); running_ = false; CoUninitialize(); return;
    }
    sender.set_sender_info("OMT Mini Desktop Capture", "OMT Mini", OMTMINI_VERSION);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.width   = width;
        stats_.height  = height;
        stats_.address = sender.address();
        stats_.audio   = cfg.capture_audio;
    }
    util::logf("capture: '%s' %dx%d from %s", name.c_str(), width, height,
               util::narrow(mon.device).c_str());

    if (cfg.capture_audio) audio_.start();

    // ---- capture loop ----
    const int    fps = std::clamp(cfg.capture_fps, 1, 120);
    const double frame_interval_ms = 1000.0 / fps;
    const int    stride = width * 4;

    std::vector<uint8_t> frame_buffer(static_cast<size_t>(stride) * height, 0);
    std::vector<uint8_t> pointer_shape;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer_info{};
    int pointer_x = 0, pointer_y = 0;
    bool pointer_visible = false;
    bool have_frame = false;

    std::vector<float> audio_planar;
    double next_frame_ms = static_cast<double>(util::now_ms());
    int64_t frames_sent = 0;
    int64_t fps_window_start = util::now_ms();
    int     fps_window_frames = 0;

    while (running_) {
        // --- pull the newest desktop update, if any ---
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        hr = dupl->AcquireNextFrame(4, &info, resource.put());

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            util::logf("capture: access lost, re-acquiring duplication");
            dupl.reset();
            if (FAILED(output1->DuplicateOutput(device.get(), dupl.put()))) {
                Sleep(200);
                continue;
            }
            continue;
        }

        if (SUCCEEDED(hr)) {
            if (info.LastPresentTime.QuadPart != 0) {
                ComPtr<ID3D11Texture2D> desktop;
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                       desktop.put_void()))) {
                    context->CopyResource(staging.get(), desktop.get());

                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
                        const auto* src = static_cast<const uint8_t*>(m.pData);
                        for (int y = 0; y < height; ++y)
                            std::memcpy(frame_buffer.data() + static_cast<size_t>(y) * stride,
                                        src + static_cast<size_t>(y) * m.RowPitch,
                                        static_cast<size_t>(stride));
                        context->Unmap(staging.get(), 0);
                        have_frame = true;
                    }
                }
            }

            // Pointer position and shape arrive alongside the frame.
            if (info.LastMouseUpdateTime.QuadPart != 0) {
                pointer_visible = info.PointerPosition.Visible != 0;
                pointer_x = info.PointerPosition.Position.x;
                pointer_y = info.PointerPosition.Position.y;
            }
            if (info.PointerShapeBufferSize > 0) {
                pointer_shape.resize(info.PointerShapeBufferSize);
                UINT required = 0;
                if (FAILED(dupl->GetFramePointerShape(info.PointerShapeBufferSize,
                                                      pointer_shape.data(), &required,
                                                      &pointer_info)))
                    pointer_shape.clear();
            }
            dupl->ReleaseFrame();
        }

        // --- send on a steady cadence, repeating the last frame when idle ---
        const double now = static_cast<double>(util::now_ms());
        if (have_frame && now >= next_frame_ms) {
            // Avoid spiralling if the machine stalls.
            next_frame_ms = (now - next_frame_ms > frame_interval_ms * 4)
                          ? now + frame_interval_ms
                          : next_frame_ms + frame_interval_ms;

            std::vector<uint8_t>* out = &frame_buffer;
            std::vector<uint8_t> composited;
            if (cfg.capture_cursor && pointer_visible && !pointer_shape.empty()) {
                composited = frame_buffer;
                composite_pointer(composited.data(), width, height, stride,
                                  pointer_shape, pointer_info, pointer_x, pointer_y);
                out = &composited;
            }

            OMTMediaFrame vf{};
            vf.Type        = OMTFrameType_Video;
            vf.Timestamp   = util::now_omt_ticks();
            vf.Codec       = OMTCodec_BGRA;   // no alpha flag, so encoded as BGRX
            vf.Width       = width;
            vf.Height      = height;
            vf.Stride      = stride;
            vf.Flags       = OMTVideoFlags_None;
            vf.FrameRateN  = fps;
            vf.FrameRateD  = 1;
            vf.AspectRatio = static_cast<float>(width) / static_cast<float>(height);
            vf.ColorSpace  = OMTColorSpace_BT709;
            vf.Data        = out->data();
            vf.DataLength  = static_cast<int>(out->size());
            sender.send(&vf);

            ++frames_sent;
            ++fps_window_frames;
        }

        // --- audio ---
        if (cfg.capture_audio && audio_.running()) {
            const int n = audio_.read_planar(&audio_planar, 4800);
            if (n > 0) {
                OMTMediaFrame af{};
                af.Type              = OMTFrameType_Audio;
                af.Timestamp         = util::now_omt_ticks();
                af.Codec             = OMTCodec_FPA1;
                af.SampleRate        = audio_.sample_rate();
                af.Channels          = audio_.channels();
                af.SamplesPerChannel = n;
                af.Data              = audio_planar.data();
                af.DataLength        = n * audio_.channels() * 4;
                sender.send(&af);
            }
        }

        // --- stats once a second ---
        const int64_t elapsed = util::now_ms() - fps_window_start;
        if (elapsed >= 1000) {
            OMTStatistics vs{};
            sender.video_stats(&vs);
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.fps         = static_cast<float>(fps_window_frames) * 1000.0f / elapsed;
            stats_.frames      = frames_sent;
            stats_.connections = sender.connections();
            stats_.bytes_sent  = vs.BytesSent;
            stats_.mbps        = static_cast<float>(vs.BytesSentSinceLast) * 8.0f /
                                 static_cast<float>(elapsed) / 1000.0f;
            stats_.address     = sender.address();
            stats_.running     = true;

            // libomt does not report which port a sender bound to, so it is
            // read back from the system once the socket is up.
            if (stats_.connect_url.empty()) {
                const int port = netinfo::port_opened_since(ports_before, cfg.port_start,
                                                            cfg.port_end);
                const std::string ip = netinfo::local_ipv4();
                if (port > 0 && !ip.empty()) {
                    stats_.connect_url = "omt://" + ip + ":" + std::to_string(port);
                    util::logf("capture: reachable at %s", stats_.connect_url.c_str());
                }
            }
            fps_window_frames = 0;
            fps_window_start  = util::now_ms();
        }

        if (!have_frame) Sleep(10);
    }

    audio_.stop();
    sender.close();
    CoUninitialize();
}
