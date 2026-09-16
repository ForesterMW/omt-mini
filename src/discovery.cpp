// Winsock has to come before anything that reaches windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "discovery.h"
#include "omt.h"
#include "settings.h"
#include "netinfo.h"
#include "announce.h"

#include <algorithm>

namespace {
Discovery g_discovery;

// Reads the product name a sender reports about itself.
//
// OMT already carries this: a sender fills in OMTSenderInfo and any receiver
// can read it back. The catch is that it is only valid while connected, so it
// cannot come from discovery alone. A metadata only receiver is the cheapest
// way to ask: no video or audio is requested, so it costs the sender one short
// connection and nothing else, and the answer is cached for good.
bool identify_sender(const std::string& address, std::string* product,
                     std::string* manufacturer) {
    omt::Receiver receiver;
    if (!receiver.open(address, OMTFrameType_Metadata,
                       OMTPreferredVideoFormat_UYVY, OMTReceiveFlags_None))
        return false;

    OMTSenderInfo info{};
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (receiver.sender_info(&info)) {
            if (product)      *product = info.ProductName;
            if (manufacturer) *manufacturer = info.Manufacturer;
            return true;
        }
        Sleep(100);
    }
    return false;
}

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
    thread_       = std::thread([this] { run(); });
    probe_thread_ = std::thread([this] { probe_run(); });
}

void Discovery::stop() {
    if (!running_.exchange(false)) return;
    wake_ = true;
    probe_now_ = true;
    if (thread_.joinable()) thread_.join();
    if (probe_thread_.joinable()) probe_thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.clear();
}

void Discovery::refresh_now() { wake_ = true; }

void Discovery::set_manual_sources(const std::vector<ManualSource>& manual) {
    std::vector<DiscoveredSource> next;
    next.reserve(manual.size());
    const int64_t now = util::now_ms();

    for (const auto& m : manual) {
        if (m.address.empty()) continue;
        DiscoveredSource s;
        s.address       = m.address;
        s.host          = omt::host_name(m.address);
        s.name          = m.name.empty() ? s.host : m.name;
        s.is_manual     = true;
        s.status        = SourceStatus::Unknown;
        s.first_seen_ms = now;
        next.push_back(std::move(s));
    }

    {
        std::lock_guard<std::mutex> lock(manual_mutex_);
        for (auto& entry : next) {
            for (const auto& old : manual_) {
                if (old.address == entry.address) { entry.status = old.status; break; }
            }
        }
        manual_.swap(next);
    }
    // Republish straight away rather than waiting for the next poll, and
    // find out whether the new entry answers.
    wake_ = true;
    probe_now_ = true;
    // What is announced follows what is in the list.
    announcer().refresh();
    if (window_) PostMessageW(window_, message_, 0, 0);
}

std::vector<DiscoveredSource> Discovery::sources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_;
}

size_t Discovery::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
}

void Discovery::probe_run() {
    WSADATA wsa{};
    const bool winsock_ready = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    if (!winsock_ready)
        util::logf("discovery: WSAStartup failed, reachability will stay unknown");
    netinfo::init();

    // Eight seconds is frequent enough to notice a source going away without
    // being something anyone would feel.
    constexpr int kIntervalMs = 8000;
    constexpr int kTimeoutMs  = 1200;
    int64_t next_probe = 0;

    while (running_) {
        if (!winsock_ready) { Sleep(200); continue; }

        if (probe_now_.exchange(false)) next_probe = 0;
        if (util::now_ms() < next_probe) {
            Sleep(100);
            continue;
        }

        std::vector<DiscoveredSource> targets;
        {
            std::lock_guard<std::mutex> lock(manual_mutex_);
            targets = manual_;
        }

        // ---- resolve host names so the list can show an address ----
        {
            std::vector<std::string> hosts;
            {
                std::lock_guard<std::mutex> sources_lock(mutex_);
                std::lock_guard<std::mutex> identity_lock(identity_mutex_);
                for (const auto& source : sources_) {
                    if (source.host.empty()) continue;
                    if (resolved_.count(source.host)) continue;
                    hosts.push_back(source.host);
                }
            }
            for (const auto& host : hosts) {
                if (!running_) break;
                const std::string address = netinfo::resolve(host);
                std::lock_guard<std::mutex> lock(identity_mutex_);
                resolved_[host] = address;
                if (!address.empty()) { status_changed_ = true; wake_ = true; }
            }
        }

        // ---- identify anything newly seen ----
        if (settings().identify_sources && omt::loaded()) {
            std::vector<std::string> unknown;
            {
                std::lock_guard<std::mutex> sources_lock(mutex_);
                std::lock_guard<std::mutex> identity_lock(identity_mutex_);
                for (const auto& source : sources_) {
                    if (identity_.count(source.address)) continue;
                    // An added source that is not answering cannot be asked.
                    if (source.is_manual && source.status == SourceStatus::Offline) continue;
                    unknown.push_back(source.address);
                }
            }

            for (const auto& address : unknown) {
                if (!running_) break;
                DiscoveredSource identity;
                identity.address = address;
                if (identify_sender(address, &identity.product, &identity.manufacturer)) {
                    identity.is_omt_mini = util::iequals(identity.manufacturer, "OMT Mini");
                    util::logf("discovery: %s identifies as '%s' by '%s'", address.c_str(),
                               identity.product.c_str(), identity.manufacturer.c_str());
                }
                {
                    std::lock_guard<std::mutex> lock(identity_mutex_);
                    identity_[address] = identity;
                }
                status_changed_ = true;
                wake_ = true;
            }
        }

        for (const auto& target : targets) {
            if (!running_) break;
            std::string host, port;
            if (!omt::split_address(target.address, &host, &port)) continue;

            const SourceStatus status = netinfo::port_open(host, port, kTimeoutMs)
                                      ? SourceStatus::Online
                                      : SourceStatus::Offline;

            std::lock_guard<std::mutex> lock(manual_mutex_);
            for (auto& entry : manual_) {
                if (entry.address != target.address) continue;
                if (entry.status != status) {
                    entry.status = status;
                    status_changed_ = true;
                    wake_ = true;
                    util::logf("discovery: %s is %s", entry.address.c_str(),
                               status == SourceStatus::Online ? "online" : "offline");
                }
                break;
            }
        }

        // Drop identities for sources that have gone, so a host that comes back
        // as something else is not remembered wrongly.
        {
            std::lock_guard<std::mutex> sources_lock(mutex_);
            std::lock_guard<std::mutex> identity_lock(identity_mutex_);
            for (auto it = identity_.begin(); it != identity_.end();) {
                const bool still_listed = std::any_of(
                    sources_.begin(), sources_.end(),
                    [&](const DiscoveredSource& s) { return s.address == it->first; });
                it = still_listed ? std::next(it) : identity_.erase(it);
            }
        }

        next_probe = util::now_ms() + kIntervalMs;
    }

    if (winsock_ready) WSACleanup();
}

void Discovery::run() {
    util::logf("discovery: thread started");

    std::vector<std::string> previous;
    std::vector<std::string> last_manual;
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

        // Our own announcements come back to us over mDNS like anything else.
        // Dropped here, before identification or the list is built, so a hand
        // added source does not also appear as a discovered one.
        const auto ours = announcer().advertised_addresses();
        if (!ours.empty()) {
            current.erase(std::remove_if(current.begin(), current.end(),
                                         [&](const std::string& address) {
                                             return std::find(ours.begin(), ours.end(),
                                                              address) != ours.end();
                                         }),
                          current.end());
        }

        bool manual_changed = false;
        {
            std::lock_guard<std::mutex> lock(manual_mutex_);
            std::vector<std::string> manual_now;
            manual_now.reserve(manual_.size());
            for (const auto& m : manual_) manual_now.push_back(m.address);
            manual_changed = (manual_now != last_manual);
            if (manual_changed) last_manual.swap(manual_now);
        }

        const bool status_changed = status_changed_.exchange(false);

        if (current != previous || manual_changed || status_changed) {
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

            // Manual entries sit alongside the discovered ones. A hand added
            // address that the network also advertises is dropped here so it
            // does not appear twice.
            {
                std::lock_guard<std::mutex> lock(manual_mutex_);
                for (const auto& m : manual_) {
                    const bool duplicate = std::any_of(
                        next.begin(), next.end(),
                        [&](const DiscoveredSource& o) { return o.address == m.address; });
                    if (!duplicate) next.push_back(m);
                }
            }

            // Attach whatever each source has told us about itself.
            {
                std::lock_guard<std::mutex> lock(identity_mutex_);
                for (auto& entry : next) {
                    auto it = identity_.find(entry.address);
                    if (it != identity_.end()) {
                        entry.product      = it->second.product;
                        entry.manufacturer = it->second.manufacturer;
                        entry.is_omt_mini  = it->second.is_omt_mini;
                    }
                    auto resolved = resolved_.find(entry.host);
                    if (resolved != resolved_.end()) entry.ip = resolved->second;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                sources_.swap(next);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                util::logf("discovery: %zu source(s) (%zu discovered)",
                           sources_.size(), current.size());
            }
            if (window_) PostMessageW(window_, message_, 0, 0);
            previous = std::move(current);
        }

        // Poll about once a second, but break out early when asked to.
        for (int i = 0; i < 20 && running_; ++i) {
            if (wake_.exchange(false)) break;
            Sleep(50);
        }
    }
    util::logf("discovery: thread stopped");
}
