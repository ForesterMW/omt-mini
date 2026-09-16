// WASAPI audio: monitoring for viewer windows, loopback capture for the
// desktop sender.
//
// These live in their own translation units because <audioclient.h> and
// <dshow.h> both define DDPIXELFORMAT and cannot be included together under
// mingw-w64.
#pragma once
#include "util.h"

#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

extern "C" {
#include "libomt.h"
}

// Plays received OMT audio on the default render device.
class AudioMonitor {
public:
    ~AudioMonitor() { stop(); }

    // Started lazily from the first audio frame; safe to call repeatedly.
    bool ensure(int sample_rate, int channels);
    void stop();
    bool running() const { return running_; }

    // Accepts a planar float OMT audio frame (FPA1).
    void push(const OMTMediaFrame& frame);

    void set_volume(float v);        // 0.0 .. 1.0
    void set_muted(bool m)  { muted_ = m; }
    bool muted() const      { return muted_; }

    // Per channel peak in the last ~100 ms, 0.0 .. 1.0+, for the level meter.
    int   meter_channels() const { return meter_channels_; }
    float peak(int channel) const;
    // Drops the metered peaks towards zero; call once per UI frame.
    void  decay_meters(float dt);

private:
    void run();
    bool open_device(int sample_rate, int channels);
    void close_device();

    mutable std::mutex  mutex_;
    std::vector<float>  ring_;            // interleaved, device channel count
    size_t              read_ = 0, write_ = 0, filled_ = 0;

    std::thread         thread_;
    std::atomic<bool>   running_{false};
    std::atomic<bool>   muted_{false};
    std::atomic<int>    volume_q15_{32768};

    int  sample_rate_ = 0;
    int  out_channels_ = 2;
    int  src_channels_ = 0;

    static constexpr int kMaxMeters = 8;
    std::atomic<int>   meter_channels_{0};
    std::atomic<int>   peaks_q15_[kMaxMeters] = {};

    void* client_ = nullptr;   // IAudioClient*
    void* render_ = nullptr;   // IAudioRenderClient*
    void* event_  = nullptr;   // HANDLE
};

// Captures the default render endpoint in loopback for the desktop sender.
class AudioLoopback {
public:
    ~AudioLoopback() { stop(); }

    bool start();
    void stop();
    bool running() const { return running_; }

    int sample_rate() const { return sample_rate_; }
    int channels() const    { return channels_; }

    // Moves captured audio into planar float layout ready for omt_send.
    // Returns the number of samples per channel written, or 0 when idle.
    int read_planar(std::vector<float>* planar, int max_samples_per_channel);

private:
    void run();

    std::mutex          mutex_;
    std::vector<float>  ring_;            // interleaved
    size_t              read_ = 0, write_ = 0, filled_ = 0;

    std::thread         thread_;
    std::atomic<bool>   running_{false};
    int sample_rate_ = 48000;
    int channels_    = 2;
};
