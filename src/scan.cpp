#include "scan.h"
#include "netinfo.h"
#include "omt.h"
#include "window.h"

#include <objbase.h>

namespace {
PortScanner g_scanner;

// Short enough that a closed port does not hold the walk up, long enough for a
// switch or two in between.
constexpr int kConnectTimeoutMs = 300;
// How long to wait for a sender to say what it is before giving up on it.
constexpr int kIdentifyMs = 1500;

// Confirms there is really an OMT sender on the other end, rather than just
// something holding the port open, and picks up its name while it is there.
//
// A metadata only receiver asks for no video and no audio, so this costs the
// sender one short connection.
bool confirm_omt_sender(const std::string& address, std::string* product) {
    omt::Receiver receiver;
    if (!receiver.open(address, OMTFrameType_Metadata,
                       OMTPreferredVideoFormat_UYVY, OMTReceiveFlags_None))
        return false;

    const int64_t deadline = util::now_ms() + kIdentifyMs;
    while (util::now_ms() < deadline) {
        OMTSenderInfo info{};
        if (receiver.sender_info(&info)) {
            if (product) *product = info.ProductName;
            return true;
        }
        Sleep(100);
    }
    return false;
}
} // namespace

PortScanner& port_scanner() { return g_scanner; }

ScanState PortScanner::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void PortScanner::start(const std::string& host, int start_port, int end_port, int gap,
                        HWND notify) {
    if (running_.exchange(true)) return;
    join();
    cancel_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ScanState{};
        state_.running = true;
        state_.host = host;
        state_.current_port = start_port;
    }
    thread_ = std::thread([this, host, start_port, end_port, gap, notify] {
        run(host, start_port, end_port, gap, notify);
        running_ = false;
    });
}

void PortScanner::cancel() { cancel_ = true; }

void PortScanner::join() {
    if (thread_.joinable()) thread_.join();
}

void PortScanner::run(std::string host, int start_port, int end_port, int gap,
                      HWND notify) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    netinfo::init();

    util::logf("scan: walking %s from %d to %d", host.c_str(), start_port, end_port);

    int misses = 0;
    int scanned = 0;
    for (int port = start_port; port <= end_port && !cancel_; ++port) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.current_port = port;
            state_.scanned = scanned;
        }
        if (notify) PostMessageW(notify, WM_OMT_SCAN, 0, 0);

        const std::string port_text = std::to_string(port);
        bool found = false;

        if (netinfo::port_open(host, port_text, kConnectTimeoutMs)) {
            const std::string address = "omt://" + host + ":" + port_text;
            std::string product;
            if (confirm_omt_sender(address, &product)) {
                ScanHit hit;
                hit.address = address;
                hit.product = product;
                hit.port = port;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_.hits.push_back(hit);
                }
                util::logf("scan: found %s (%s)", address.c_str(), product.c_str());
                found = true;
            }
        }

        ++scanned;

        // The budget resets on every find, so a machine with senders spread
        // across the range is still walked all the way out.
        misses = found ? 0 : misses + 1;
        if (misses >= gap) break;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.running = false;
        state_.finished = true;
        state_.scanned = scanned;
    }
    util::logf("scan: finished, %d port(s) checked", scanned);
    if (notify) PostMessageW(notify, WM_OMT_SCAN, 1, 0);

    CoUninitialize();
}
