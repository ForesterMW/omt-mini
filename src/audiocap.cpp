#include "audio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <algorithm>
#include <cstring>

static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static const GUID kIID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID kIID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

namespace {
constexpr int kBufferMs = 500;
IAudioClient*        g_client  = nullptr;
IAudioCaptureClient* g_capture = nullptr;
} // namespace

bool AudioLoopback::start() {
    if (running_) return true;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kIID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return false;

    IMMDevice* device = nullptr;
    // Loopback captures what the default render endpoint is playing.
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return false;

    IAudioClient* client = nullptr;
    hr = device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr) || !client) return false;

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) {
        client->Release();
        return false;
    }

    sample_rate_ = static_cast<int>(mix->nSamplesPerSec);
    channels_    = std::min<int>(2, mix->nChannels);

    // Shared mode loopback always hands back the device mix format, which is
    // float32 on every current Windows build.
    const bool is_float = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    if (!is_float) {
        util::logf("audiocap: unsupported mix format tag=%u bits=%u",
                   mix->wFormatTag, mix->wBitsPerSample);
        CoTaskMemFree(mix);
        client->Release();
        return false;
    }

    const int mix_channels = mix->nChannels;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                            200 * 10000, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        util::logf("audiocap: Initialize hr=0x%08lx", static_cast<unsigned long>(hr));
        client->Release();
        return false;
    }

    IAudioCaptureClient* capture = nullptr;
    hr = client->GetService(kIID_IAudioCaptureClient, reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        client->Release();
        return false;
    }

    g_client  = client;
    g_capture = capture;
    channels_ = std::min(2, mix_channels);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.assign(static_cast<size_t>(sample_rate_) * channels_ * kBufferMs / 1000, 0.0f);
        read_ = write_ = filled_ = 0;
    }

    client->Start();
    running_ = true;
    thread_  = std::thread([this] { run(); });
    util::logf("audiocap: loopback started %d Hz, %d ch", sample_rate_, channels_);
    return true;
}

void AudioLoopback::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();

    if (g_client)  { g_client->Stop(); g_client->Release(); g_client = nullptr; }
    if (g_capture) { g_capture->Release(); g_capture = nullptr; }

    std::lock_guard<std::mutex> lock(mutex_);
    ring_.clear();
    read_ = write_ = filled_ = 0;
    util::logf("audiocap: stopped");
}

void AudioLoopback::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    // Loopback has no event handle in shared mode, so poll at a short interval.
    while (running_) {
        Sleep(5);

        UINT32 packet = 0;
        if (!g_capture || FAILED(g_capture->GetNextPacketSize(&packet))) continue;

        while (packet > 0 && running_) {
            BYTE*  data = nullptr;
            UINT32 frames = 0;
            DWORD  flags = 0;
            if (FAILED(g_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (frames > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ring_.empty()) {
                    const size_t need = static_cast<size_t>(frames) * channels_;
                    if (filled_ + need > ring_.size()) {
                        const size_t drop = filled_ + need - ring_.size();
                        read_   = (read_ + drop) % ring_.size();
                        filled_ -= drop;
                    }
                    const auto* src = reinterpret_cast<const float*>(data);
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    for (UINT32 i = 0; i < frames; ++i) {
                        for (int c = 0; c < channels_; ++c) {
                            ring_[write_] = silent ? 0.0f : src[i * channels_ + c];
                            write_ = (write_ + 1) % ring_.size();
                        }
                    }
                    filled_ += need;
                }
            }

            g_capture->ReleaseBuffer(frames);
            if (FAILED(g_capture->GetNextPacketSize(&packet))) break;
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

int AudioLoopback::read_planar(std::vector<float>* planar, int max_samples_per_channel) {
    if (!planar || max_samples_per_channel <= 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty() || filled_ == 0) return 0;

    const int have = static_cast<int>(filled_ / channels_);
    const int n    = std::min(have, max_samples_per_channel);
    if (n <= 0) return 0;

    // OMT wants planar float: all of channel 0, then all of channel 1.
    planar->resize(static_cast<size_t>(n) * channels_);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < channels_; ++c) {
            (*planar)[static_cast<size_t>(c) * n + i] = ring_[read_];
            read_ = (read_ + 1) % ring_.size();
        }
    }
    filled_ -= static_cast<size_t>(n) * channels_;
    return n;
}
