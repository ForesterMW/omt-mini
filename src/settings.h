// Persistent configuration, stored as INI at %APPDATA%\OMT Mini\settings.ini.
//
// A flat INI keeps the file hand-editable and the parser tiny; there is no
// nesting in this config worth a JSON dependency.
#pragma once
#include <string>

extern "C" {
#include "libomt.h"
}

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
    int         capture_fps       = 30;
    int         capture_quality   = OMTQuality_Default;
    bool        capture_cursor    = true;
    bool        capture_audio     = true;     // WASAPI loopback of default device
    bool        capture_autostart = false;

    // ---- Webcam output ----
    std::string webcam_source;                // OMT address to feed the vcam
    int         webcam_width      = 1280;
    int         webcam_height     = 720;
    int         webcam_fps        = 30;
    bool        webcam_autostart  = false;

    // ---- Window placement (remembered, not user facing) ----
    int         sources_x = -1, sources_y = -1;

    void load();
    void save() const;

    // Pushes discovery_server / port range into libomt for this process.
    void apply_to_libomt() const;
};

Settings& settings();
