// Background OMT source discovery.
//
// libomt exposes discovery as a snapshot call whose returned array is only
// valid until the next call, so this polls on its own thread, copies the
// strings, and posts a message to the UI when the set changes.
#pragma once
#include "util.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

struct ManualSource;

struct DiscoveredSource {
    std::string address;    // "HOSTNAME (Source Name)" as advertised
    std::string name;       // display name
    std::string host;
    int64_t     first_seen_ms = 0;
    bool        is_local = false;   // advertised by this machine
    bool        is_manual = false;  // added by hand, not discovered
};

class Discovery {
public:
    // notify_window receives notify_msg whenever the source list changes.
    void start(HWND notify_window, UINT notify_msg);
    void stop();

    std::vector<DiscoveredSource> sources() const;
    size_t count() const;
    // Forces a poll on the next loop iteration instead of waiting the interval.
    void refresh_now();

    // Hand entered senders, merged into the list alongside discovered ones.
    // Call whenever the settings change.
    void set_manual_sources(const std::vector<ManualSource>& manual);

private:
    void run();

    mutable std::mutex            mutex_;
    std::vector<DiscoveredSource> sources_;
    std::thread                   thread_;
    std::atomic<bool>             running_{false};
    std::atomic<bool>             wake_{false};
    HWND                          window_ = nullptr;
    UINT                          message_ = 0;
    std::string                   local_host_;

    mutable std::mutex            manual_mutex_;
    std::vector<DiscoveredSource> manual_;
};

Discovery& discovery();
