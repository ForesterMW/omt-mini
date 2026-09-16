// One click update from the latest stable GitHub release.
//
// Deliberately split into two steps. Checking is a read only network call and
// may happen on launch; installing NEVER happens on its own, only when someone
// presses the button. A machine that is on air does not get restarted out from
// under its operator.
//
// The repository is public, so no token is involved and no credential is ever
// read, stored or sent.
#pragma once
#include "util.h"

#include <string>
#include <atomic>
#include <thread>
#include <mutex>

enum class UpdateState {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    ReadyToInstall,
    Failed,
};

struct UpdateInfo {
    UpdateState state = UpdateState::Idle;
    std::string latest_version;       // "0.2.0"
    std::string current_version;
    std::string release_url;          // human readable release page
    std::string setup_url;            // installer asset
    std::string checksums_url;        // SHA256SUMS asset
    std::string setup_name;
    int64_t     setup_size = 0;
    int         progress_percent = 0;
    std::string error;
    std::wstring downloaded_path;
};

class Updater {
public:
    ~Updater() { join(); }

    // Asks GitHub for the latest stable release. Non blocking; the window is
    // notified with WM_OMT_UPDATE when the state changes.
    void check(HWND notify, bool quiet = false);

    // Downloads the installer, verifies it against the published SHA256SUMS,
    // and runs it. Only ever called from an explicit user action.
    void download_and_install(HWND notify);

    UpdateInfo info() const;
    bool busy() const;
    // Abandons an in flight transfer so shutdown is not held up by a slow
    // download. Safe to call when nothing is running.
    void cancel();
    void join();

    // True when a newer stable release than this build is available.
    bool update_available() const;

    static int compare_versions(const std::string& a, const std::string& b);

private:
    void set(const UpdateInfo& next);
    void run_check(HWND notify, bool quiet);
    void run_install(HWND notify);

    mutable std::mutex mutex_;
    UpdateInfo         info_;
    std::thread        thread_;
    std::atomic<bool>  busy_{false};
    std::atomic<bool>  cancel_{false};
};

Updater& updater();
