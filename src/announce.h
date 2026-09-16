// Announcing hand added sources on the local network.
//
// A source added by address is, by definition, one this network cannot see.
// vMix and everything else find OMT sources by browsing mDNS, so to make one
// visible to them it has to be announced there. libomt announces every sender
// it creates over DNS-SD, so a sender is the announcement.
//
// No video passes through here. Each announcement is a sender that carries
// nothing and redirects to the real machine, which is what OMT calls a virtual
// source. A receiver that picks it up is told to go and fetch the pictures
// from the original host directly.
//
// Only hand added sources are announced. Anything already discovered is
// already on mDNS, so re-announcing it would put it on the network twice, and
// a machine that receives one of these announcements sees it as a discovered
// source rather than a hand added one, so it will not announce it again.
#pragma once
#include "util.h"

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

struct AnnouncedSource {
    std::string target;       // omt://host:port that receivers are sent to
    std::string advertised;   // HOSTNAME (Name) as it appears on the network
    std::string name;
};

class SourceAnnouncer {
public:
    static SourceAnnouncer& instance();

    void start();
    void stop();
    // Reconcile against the current settings. Cheap and safe to call often.
    void refresh();

    // What this machine is currently putting on the network, so the source
    // list can filter its own announcements back out of discovery.
    std::vector<std::string> advertised_addresses() const;
    std::vector<AnnouncedSource> announced() const;
    bool is_announced(const std::string& target) const;
    size_t count() const;

    // Whether a given hand added source should be announced, given the global
    // setting and its own override.
    static bool wants_announce(const struct ManualSource& source);

private:
    SourceAnnouncer() = default;
    void run();
    void reconcile();
    void shutdown_all();

    mutable std::mutex mutex_;
    struct Entry;
    std::vector<std::unique_ptr<Entry>> entries_;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> dirty_{true};
};

SourceAnnouncer& announcer();
