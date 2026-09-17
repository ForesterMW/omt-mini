// Persistent configuration, stored as INI at %APPDATA%\OMT Mini\settings.ini.
//
// A flat INI keeps the file hand-editable and the parser tiny; there is no
// nesting in this config worth a JSON dependency.
#pragma once
#include <string>
#include <vector>

extern "C" {
#include "libomt.h"
}

// One hand entered sender.
struct ManualSource {
    std::string address;   // omt://host:port
    std::string name;      // optional label, falls back to the address
    // Consulted only when announce_manual_sources is off.
    bool        announce = true;
};

struct Settings {
    // ---- General ----
    bool        run_at_login      = false;
    bool        show_sources_on_start = true;
    bool        notify_on_source_change = false;
    bool        confirm_exit      = true;

    // ---- Network (mapped onto libomt's own settings) ----
    std::string discovery_server;             // omt://host:port, blank = DNS-SD
    int         port_start        = 6400;
    int         port_end          = 6600;

    // ---- Viewer defaults ----
    int         viewer_quality    = OMTQuality_Default;
    int         viewer_format     = OMTPreferredVideoFormat_UYVYorBGRA;
    bool        viewer_audio      = true;
    int         viewer_volume     = 100;      // 0..100
    bool        viewer_show_stats = false;
    bool        viewer_always_on_top = false;
    bool        viewer_low_bandwidth = false; // request 1/8 preview stream
    int         viewer_width      = 960;
    int         viewer_height     = 540;

    // ---- Desktop capture ----
    std::string capture_name      = "Desktop";
    int         capture_monitor   = 0;        // index into enumerated outputs
    int         capture_fps       = 50;
    int         capture_quality   = OMTQuality_Default;
    bool        capture_cursor    = true;
    bool        capture_audio     = true;     // WASAPI loopback of default device
    bool        capture_autostart = false;

    // ---- Multiview ----
    int         multiview_layout    = 0;      // index into multiview_layouts()
    bool        multiview_preview   = true;   // ask senders for their 1/8 stream
    bool        multiview_autostart = false;
    bool        multiview_fullscreen = false;
    std::vector<std::string> multiview_sources;   // one per tile, may be empty

    // The multiview as an OMT source for other machines. Runs with or without
    // the window, which is what makes a headless wall possible.
    bool        multiview_output_enabled = false;
    std::string multiview_output_name = "Multiview";
    int         multiview_output_width  = 1920;
    int         multiview_output_height = 1080;
    int         multiview_output_fps    = 25;

    // ---- Webcam output ----
    std::string webcam_source;                // OMT address to feed the vcam
    int         webcam_width      = 1280;
    int         webcam_height     = 720;
    int         webcam_fps        = 30;
    bool        webcam_autostart  = false;

    // ---- Manually added sources ----
    // Senders that discovery cannot see: a different subnet, a VPN, or a host
    // with mDNS blocked. Addressed directly as omt://host:port, which is what
    // libomt accepts in place of a discovered name.
    std::vector<ManualSource> manual_sources;

    // Put hand added sources on this network over mDNS, so vMix and anything
    // else that browses for OMT sources can see them. Announcements carry no
    // media: they redirect to the machine the source actually lives on.
    bool        announce_manual_sources = true;

    // Briefly connects to each newly seen source, requesting metadata only, to
    // read the product name it reports. Costs the sender one short connection
    // per source, once, and nothing after that.
    bool        identify_sources = true;

    // ---- Control panel ----
    // Off unless asked for: it can close a window on a machine that is on air,
    // so it should not start listening on a network without being turned on.
    bool        web_enabled = false;
    int         web_port = 7400;

    // ---- Updates ----
    // Checking is a read only call to the public GitHub API. Installing is
    // never automatic and always needs an explicit press.
    bool        check_updates_on_launch = true;

    // ---- Window placement (remembered, not user facing) ----
    int         sources_x = -1, sources_y = -1;

    void load();
    void save() const;

    // Pushes discovery_server / port range into libomt for this process.
    void apply_to_libomt() const;
};

Settings& settings();

// What this copy tells other OMT Minis about itself when they connect. Empty
// while the control panel is off, so a machine that has not opted into remote
// control does not advertise one.
std::string omtmini_control_metadata();
