#include "announce.h"
#include "omt.h"
#include "settings.h"

#include <objbase.h>
#include <algorithm>
#include <memory>

struct SourceAnnouncer::Entry {
    omt::Sender sender;
    std::string target;
    std::string advertised;
    std::string name;
};

namespace {
// The name this machine puts on the network for a source. Falls back to the
// host, because "10.0.0.5" on the network is more use than nothing.
std::string announce_name(const ManualSource& source) {
    // Falls back to the machine it lives on, because a source with no name is
    // one other applications will not list.
    return omt::safe_source_name(source.name, omt::host_only(source.address));
}
} // namespace

SourceAnnouncer& SourceAnnouncer::instance() {
    static SourceAnnouncer announcer;
    return announcer;
}
SourceAnnouncer& announcer() { return SourceAnnouncer::instance(); }

bool SourceAnnouncer::wants_announce(const ManualSource& source) {
    const Settings& cfg = settings();
    // The global setting is the default for everything; the per source flag is
    // only consulted once the global one is off.
    return cfg.announce_manual_sources ? true : source.announce;
}

void SourceAnnouncer::start() {
    if (running_.exchange(true)) return;
    dirty_ = true;
    thread_ = std::thread([this] { run(); });
}

void SourceAnnouncer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    shutdown_all();
}

void SourceAnnouncer::refresh() { dirty_ = true; }

void SourceAnnouncer::shutdown_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entries_.empty())
        util::logf("announce: withdrawing %zu announcement(s)", entries_.size());
    entries_.clear();
}

std::vector<std::string> SourceAnnouncer::advertised_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_)
        if (!entry->advertised.empty()) out.push_back(entry->advertised);
    return out;
}

std::vector<AnnouncedSource> SourceAnnouncer::announced() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AnnouncedSource> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_)
        out.push_back({ entry->target, entry->advertised, entry->name });
    return out;
}

bool SourceAnnouncer::is_announced(const std::string& target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const std::unique_ptr<Entry>& e) { return e->target == target; });
}

size_t SourceAnnouncer::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void SourceAnnouncer::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    util::logf("announce: thread started");

    while (running_) {
        if (dirty_.exchange(false)) reconcile();
        for (int i = 0; i < 10 && running_ && !dirty_; ++i) Sleep(50);
    }

    shutdown_all();
    util::logf("announce: thread stopped");
    CoUninitialize();
}

void SourceAnnouncer::reconcile() {
    if (!omt::loaded()) return;

    // What should be on the network, taken as a snapshot of the settings.
    struct Wanted {
        std::string target;
        std::string name;
    };
    std::vector<Wanted> wanted;
    {
        const Settings& cfg = settings();
        std::vector<std::string> used_names;
        for (const auto& source : cfg.manual_sources) {
            if (source.address.empty()) continue;
            if (!wants_announce(source)) continue;

            Wanted item;
            item.target = source.address;
            item.name   = announce_name(source);

            // Two sources sharing a name would put two identically named
            // things on the network, so the later one carries its port.
            if (std::find(used_names.begin(), used_names.end(), item.name) !=
                used_names.end()) {
                std::string host, port;
                if (omt::split_address(source.address, &host, &port))
                    item.name += " " + port;
            }
            used_names.push_back(item.name);
            wanted.push_back(std::move(item));
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Withdraw anything no longer wanted, or whose name has changed.
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const std::unique_ptr<Entry>& entry) {
                           const bool keep = std::any_of(
                               wanted.begin(), wanted.end(), [&](const Wanted& w) {
                                   return w.target == entry->target && w.name == entry->name;
                               });
                           if (!keep)
                               util::logf("announce: withdrawing '%s'", entry->name.c_str());
                           return !keep;
                       }),
        entries_.end());

    // Announce anything new.
    for (const auto& item : wanted) {
        const bool present = std::any_of(
            entries_.begin(), entries_.end(), [&](const std::unique_ptr<Entry>& entry) {
                return entry->target == item.target && entry->name == item.name;
            });
        if (present) continue;

        auto entry = std::make_unique<Entry>();
        entry->target = item.target;
        entry->name   = item.name;

        if (!entry->sender.open(item.name, OMTQuality_Default)) {
            util::logf("announce: could not announce '%s'", item.name.c_str());
            continue;
        }
        // Carries no media of its own: anything that connects is sent straight
        // on to the machine the source actually lives on.
        entry->sender.set_redirect(item.target);
        entry->sender.set_sender_info("OMT Mini Announcement", "OMT Mini", OMTMINI_VERSION);
        entry->advertised = entry->sender.address();

        util::logf("announce: '%s' -> %s (as %s)", item.name.c_str(), item.target.c_str(),
                   entry->advertised.c_str());
        entries_.push_back(std::move(entry));
    }
}
