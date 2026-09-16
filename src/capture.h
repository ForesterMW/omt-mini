// Desktop capture source: DXGI Desktop Duplication into an OMT sender.
//
// Runs on its own thread with its own D3D11 device. Sharing the UI device
// would mean sharing an immediate context, which is not free threaded.
#pragma once
#include "util.h"
#include "audio.h"

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

struct CaptureMonitor {
    std::wstring name;        // "\\.\DISPLAY1 (2560x1440)"
    std::wstring device;
    int width = 0, height = 0;
    int adapter = 0, output = 0;
    bool primary = false;
};

struct CaptureStats {
    bool     running = false;
    int      width = 0, height = 0;
    float    fps = 0.0f;
    int      connections = 0;
    int64_t  frames = 0;
    int64_t  bytes_sent = 0;
    float    mbps = 0.0f;
    bool     audio = false;
    std::string address;      // full OMT address once announced
    std::string error;
};

class DesktopCapture {
public:
    ~DesktopCapture() { stop(); }

    static std::vector<CaptureMonitor> enumerate_monitors();

    bool start();
    void stop();
    bool running() const { return running_; }
    CaptureStats stats() const;

private:
    void run();

    std::thread        thread_;
    std::atomic<bool>  running_{false};
    mutable std::mutex stats_mutex_;
    CaptureStats       stats_;
    AudioLoopback      audio_;
};

DesktopCapture& desktop_capture();
