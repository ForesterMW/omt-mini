#include "discovery.h"
#include "omt.h"

#include <algorithm>

namespace {
Discovery g_discovery;

std::string this_hostname() {
    wchar_t buf[256] = {};
    DWORD n = ARRAYSIZE(buf);
    if (GetComputerNameW(buf, &n)) return util::narrow(std::wstring(buf, n));
    return {};
}
} // namespace

Discovery& discovery() { return g_discovery; }

void Discovery::start(HWND notify_window, UINT notify_msg) {
    if (running_.exchange(true)) return;
    window_     = notify_window;
    message_    = notify_msg;
    local_host_ = this_hostname();
    thread_     = std::thread([this] { run(); });
}

void Discovery::stop() {
    if (!running_.exchange(false)) return;
    wake_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.clear();
}

void Discovery::refresh_now() { wake_ = true; }

std::vector<DiscoveredSource> Discovery::sources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_;
}

size_t Discovery::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
}

void Discovery::run() {
    util::logf("discovery: thread started");

    std::vector<std::string> previous;
    while (running_) {
        std::vector<std::string> current;

        if (omt::loaded()) {
            int count = 0;
            char** list = omt::api().discovery_getaddresses(&count);
            if (list && count > 0) {
                current.reserve(static_cast<size_t>(count));
                for (int i = 0; i < count; ++i)
                    if (list[i] && list[i][0]) current.emplace_back(list[i]);
            }
        }

        std::sort(current.begin(), current.end());
        current.erase(std::unique(current.begin(), current.end()), current.end());

        if (current != previous) {
            const int64_t now = util::now_ms();
            std::vector<DiscoveredSource> next;
            next.reserve(current.size());

            for (const auto& addr : current) {
                DiscoveredSource s;
                s.address = addr;
                s.name    = omt::short_name(addr);
                s.host    = omt::host_name(addr);
                s.is_local = !local_host_.empty() && util::iequals(s.host, local_host_);

                // Preserve first_seen across polls so the UI can show age.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = std::find_if(sources_.begin(), sources_.end(),
                                           [&](const DiscoveredSource& o) { return o.address == addr; });
                    s.first_seen_ms = (it != sources_.end()) ? it->first_seen_ms : now;
                }
                next.push_back(std::move(s));
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                sources_.swap(next);
            }
            util::logf("discovery: %zu source(s)", current.size());
            if (window_) PostMessageW(window_, message_, 0, 0);
            previous.swap(current);
        }

        // Poll about once a second, but break out early when asked to.
        for (int i = 0; i < 20 && running_; ++i) {
            if (wake_.exchange(false)) break;
            Sleep(50);
        }
    }
    util::logf("discovery: thread stopped");
}
