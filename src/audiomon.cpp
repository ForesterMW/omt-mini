#include "audio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// Declared explicitly so the build does not depend on which mingw-w64 release
// happens to carry these in its import libraries.
static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static const GUID kIID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID kIID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace {
constexpr int kBufferMs = 400;   // ring capacity

float q15_to_float(int q) { return static_cast<float>(q) / 32768.0f; }
int   float_to_q15(float f) { return static_cast<int>(std::lround(std::clamp(f, 0.0f, 4.0f) * 32768.0f)); }
} // namespace

bool AudioMonitor::open_device(int sample_rate, int channels) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kIID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        util::logf("audiomon: CoCreateInstance hr=0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        util::logf("audiomon: no default render endpoint hr=0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }

    IAudioClient* client = nullptr;
    hr = device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr) || !client) {
        util::logf("audiomon: Activate hr=0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }

    // Feed the device float32 at the source rate and let WASAPI resample.
    out_channels_ = std::min(2, std::max(1, channels));

    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = static_cast<WORD>(out_channels_);
    wfx.Format.nSamplesPerSec  = static_cast<DWORD>(sample_rate);
    wfx.Format.wBitsPerSample  = 32;
    wfx.Format.nBlockAlign     = static_cast<WORD>(out_channels_ * 4);
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask          = (out_channels_ == 1) ? SPEAKER_FRONT_CENTER
                                                      : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfx.SubFormat              = kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const REFERENCE_TIME duration = 20 * 10000;   // 20 ms

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                            reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    if (FAILED(hr)) {
        // Older stacks reject AUTOCONVERTPCM; fall back to the mix format rate.
        util::logf("audiomon: Initialize hr=0x%08lx, retrying at device rate",
                   static_cast<unsigned long>(hr));
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            sample_rate   = static_cast<int>(mix->nSamplesPerSec);
            out_channels_ = std::min<int>(2, mix->nChannels);
            wfx.Format.nChannels       = static_cast<WORD>(out_channels_);
            wfx.Format.nSamplesPerSec  = static_cast<DWORD>(sample_rate);
            wfx.Format.nBlockAlign     = static_cast<WORD>(out_channels_ * 4);
            wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
            wfx.dwChannelMask = (out_channels_ == 1) ? SPEAKER_FRONT_CENTER
                                                     : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
            CoTaskMemFree(mix);
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0,
                                    reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
        }
        if (FAILED(hr)) {
            util::logf("audiomon: Initialize failed hr=0x%08lx", static_cast<unsigned long>(hr));
            client->Release();
            return false;
        }
    }

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev || FAILED(client->SetEventHandle(ev))) {
        if (ev) CloseHandle(ev);
        client->Release();
        return false;
    }

    IAudioRenderClient* render = nullptr;
    hr = client->GetService(kIID_IAudioRenderClient, reinterpret_cast<void**>(&render));
    if (FAILED(hr) || !render) {
        CloseHandle(ev);
        client->Release();
        return false;
    }

    client_ = client;
    render_ = render;
    event_  = ev;
    sample_rate_ = sample_rate;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.assign(static_cast<size_t>(sample_rate_) * out_channels_ * kBufferMs / 1000, 0.0f);
        read_ = write_ = filled_ = 0;
    }

    client->Start();
    util::logf("audiomon: started %d Hz, %d ch", sample_rate_, out_channels_);
    return true;
}

void AudioMonitor::close_device() {
    auto* client = static_cast<IAudioClient*>(client_);
    auto* render = static_cast<IAudioRenderClient*>(render_);
    if (client) client->Stop();
    if (render) render->Release();
    if (client) client->Release();
    if (event_) CloseHandle(static_cast<HANDLE>(event_));
    client_ = render_ = nullptr;
    event_  = nullptr;
}

bool AudioMonitor::ensure(int sample_rate, int channels) {
    if (running_) {
        // A source that changes rate mid stream needs the device reopened.
        if (sample_rate == sample_rate_) return true;
        stop();
    }
    if (sample_rate <= 0 || channels <= 0) return false;

    src_channels_ = channels;
    meter_channels_ = std::min(channels, kMaxMeters);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!open_device(sample_rate, channels)) {
        close_device();
        return false;
    }

    running_ = true;
    thread_  = std::thread([this] { run(); });
    return true;
}

void AudioMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (event_) SetEvent(static_cast<HANDLE>(event_));
    if (thread_.joinable()) thread_.join();
    close_device();
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.clear();
    read_ = write_ = filled_ = 0;
    util::logf("audiomon: stopped");
}

void AudioMonitor::set_volume(float v) {
    volume_q15_ = float_to_q15(std::clamp(v, 0.0f, 1.0f));
}

float AudioMonitor::peak(int channel) const {
    if (channel < 0 || channel >= kMaxMeters) return 0.0f;
    return q15_to_float(peaks_q15_[channel].load());
}

void AudioMonitor::decay_meters(float dt) {
    const float factor = std::max(0.0f, 1.0f - dt * 3.0f);
    for (int i = 0; i < kMaxMeters; ++i) {
        const int cur = peaks_q15_[i].load();
        peaks_q15_[i].store(static_cast<int>(cur * factor));
    }
}

void AudioMonitor::push(const OMTMediaFrame& frame) {
    if (frame.Type != OMTFrameType_Audio || !frame.Data) return;
    if (frame.Channels <= 0 || frame.SamplesPerChannel <= 0) return;

    if (!ensure(frame.SampleRate, frame.Channels)) return;

    const int   n     = frame.SamplesPerChannel;
    const int   chans = frame.Channels;
    const auto* base  = static_cast<const float*>(frame.Data);

    // Peak meter is computed from the source channels, before downmix.
    const int meters = std::min(chans, kMaxMeters);
    for (int c = 0; c < meters; ++c) {
        const float* plane = base + static_cast<size_t>(c) * n;
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(plane[i]));
        const int q = float_to_q15(peak);
        if (q > peaks_q15_[c].load()) peaks_q15_[c].store(q);
    }

    if (muted_) return;

    const float gain = q15_to_float(volume_q15_.load());

    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty()) return;

    const size_t need = static_cast<size_t>(n) * out_channels_;
    // Drop the oldest audio rather than block if the device stalls.
    if (filled_ + need > ring_.size()) {
        const size_t drop = filled_ + need - ring_.size();
        read_   = (read_ + drop) % ring_.size();
        filled_ -= drop;
    }

    const float* l = base;
    const float* r = (chans > 1) ? base + n : base;

    for (int i = 0; i < n; ++i) {
        if (out_channels_ == 1) {
            ring_[write_] = l[i] * gain;
            write_ = (write_ + 1) % ring_.size();
        } else {
            ring_[write_] = l[i] * gain;
            write_ = (write_ + 1) % ring_.size();
            ring_[write_] = r[i] * gain;
            write_ = (write_ + 1) % ring_.size();
        }
    }
    filled_ += need;
}

void AudioMonitor::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Audio threads want MMCSS priority so a busy UI cannot glitch playback.
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    auto* client = static_cast<IAudioClient*>(client_);
    auto* render = static_cast<IAudioRenderClient*>(render_);
    auto  ev     = static_cast<HANDLE>(event_);

    UINT32 buffer_frames = 0;
    if (!client || FAILED(client->GetBufferSize(&buffer_frames))) buffer_frames = 0;

    while (running_) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
        if (!running_) break;

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) continue;

        UINT32 avail = buffer_frames - padding;
        if (avail == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(avail, &out)) || !out) continue;

        auto* dst = reinterpret_cast<float*>(out);
        const size_t want = static_cast<size_t>(avail) * out_channels_;
        size_t wrote = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t take = std::min(want, filled_);
            for (size_t i = 0; i < take; ++i) {
                dst[i] = ring_[read_];
                read_  = (read_ + 1) % ring_.size();
            }
            filled_ -= take;
            wrote = take;
        }
        // Underrun: pad with silence so the stream keeps running smoothly.
        for (size_t i = wrote; i < want; ++i) dst[i] = 0.0f;

        render->ReleaseBuffer(avail, wrote == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}
