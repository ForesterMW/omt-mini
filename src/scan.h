// Finding every OMT sender on one machine.
//
// A host running several senders puts them on consecutive ports from the
// configured range start, so when someone types an address without a port
// there is usually more there than the one. This walks upward from the start
// port and keeps going as long as it keeps finding things.
#pragma once
#include "util.h"

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

struct ScanHit {
    std::string address;      // omt://host:port
    std::string product;      // what the sender reports itself to be
    int         port = 0;
};

struct ScanState {
    bool     running = false;
    bool     finished = false;
    int      current_port = 0;
    int      scanned = 0;
    std::string host;
    std::vector<ScanHit> hits;
};

class PortScanner {
public:
    ~PortScanner() { cancel(); join(); }

    // Walks from start_port upward. Gives up after `gap` consecutive ports with
    // nothing on them, and that budget resets every time a sender is found, so
    // a machine with senders spread out is still fully covered. Never goes past
    // end_port. notify receives WM_OMT_SCAN as it progresses.
    void start(const std::string& host, int start_port, int end_port, int gap,
               HWND notify);
    void cancel();
    void join();

    bool running() const { return running_; }
    ScanState state() const;

private:
    void run(std::string host, int start_port, int end_port, int gap, HWND notify);

    mutable std::mutex mutex_;
    ScanState          state_;
    std::thread        thread_;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  cancel_{false};
};

PortScanner& port_scanner();
