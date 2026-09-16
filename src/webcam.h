// Webcam output: receives one OMT source and publishes it to the OMT Mini
// virtual camera so other applications can use it as a capture device.
#pragma once
#include "util.h"
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

struct WebcamStats {
    bool        running = false;
    bool        connected = false;      // receiving frames from the source
    int         width = 0, height = 0;
    float       fps = 0.0f;
    int64_t     frames = 0;
    std::string source;
    std::string error;
};

class WebcamOutput {
public:
    ~WebcamOutput() { stop(); }

    bool start(const std::string& source);
    void stop();
    bool running() const { return running_; }
    WebcamStats stats() const;

    // True when the DirectShow filter is registered for the current user.
    static bool filter_registered();
    // Registers or unregisters the filter under HKCU. No elevation required.
    static bool register_filter(bool enable, std::wstring* message);
    static std::wstring filter_path();

private:
    void run(std::string source);

    std::thread        thread_;
    std::atomic<bool>  running_{false};
    mutable std::mutex stats_mutex_;
    WebcamStats        stats_;
};

WebcamOutput& webcam();
