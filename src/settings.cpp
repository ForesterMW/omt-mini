#include "settings.h"
#include "omt.h"
#include "util.h"

#include <map>
#include <string>
#include <cstdio>

namespace {

Settings g_settings;

std::wstring ini_path() { return util::config_path(L"settings.ini"); }

// Minimal INI: "key=value" lines, '#' or ';' comments, no sections.
std::map<std::string, std::string> read_ini() {
    std::map<std::string, std::string> kv;

    HANDLE h = CreateFileW(ini_path().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return kv;

    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        text.append(buf, got);
    CloseHandle(h);

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = util::trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[util::trim(line.substr(0, eq))] = util::trim(line.substr(eq + 1));
    }
    return kv;
}

int get_int(const std::map<std::string, std::string>& kv, const char* key, int def) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}
bool get_bool(const std::map<std::string, std::string>& kv, const char* key, bool def) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    return it->second == "1" || util::iequals(it->second, "true");
}
std::string get_str(const std::map<std::string, std::string>& kv, const char* key,
                    const std::string& def) {
    auto it = kv.find(key);
    return it == kv.end() ? def : it->second;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

Settings& settings() { return g_settings; }

void Settings::load() {
    const auto kv = read_ini();
    if (kv.empty()) return;

    run_at_login            = get_bool(kv, "run_at_login", run_at_login);
    show_sources_on_start   = get_bool(kv, "show_sources_on_start", show_sources_on_start);
    notify_on_source_change = get_bool(kv, "notify_on_source_change", notify_on_source_change);
    confirm_exit            = get_bool(kv, "confirm_exit", confirm_exit);

    discovery_server        = get_str(kv, "discovery_server", discovery_server);
    port_start              = clampi(get_int(kv, "port_start", port_start), 1024, 65535);
    port_end                = clampi(get_int(kv, "port_end", port_end), port_start, 65535);

    viewer_quality          = get_int(kv, "viewer_quality", viewer_quality);
    viewer_format           = clampi(get_int(kv, "viewer_format", viewer_format), 0, 5);
    viewer_audio            = get_bool(kv, "viewer_audio", viewer_audio);
    viewer_volume           = clampi(get_int(kv, "viewer_volume", viewer_volume), 0, 100);
    viewer_show_stats       = get_bool(kv, "viewer_show_stats", viewer_show_stats);
    viewer_always_on_top    = get_bool(kv, "viewer_always_on_top", viewer_always_on_top);
    viewer_low_bandwidth    = get_bool(kv, "viewer_low_bandwidth", viewer_low_bandwidth);
    viewer_width            = clampi(get_int(kv, "viewer_width", viewer_width), 320, 7680);
    viewer_height           = clampi(get_int(kv, "viewer_height", viewer_height), 180, 4320);

    capture_name            = get_str(kv, "capture_name", capture_name);
    capture_monitor         = clampi(get_int(kv, "capture_monitor", capture_monitor), 0, 31);
    capture_fps             = clampi(get_int(kv, "capture_fps", capture_fps), 1, 120);
    capture_quality         = get_int(kv, "capture_quality", capture_quality);
    capture_cursor          = get_bool(kv, "capture_cursor", capture_cursor);
    capture_audio           = get_bool(kv, "capture_audio", capture_audio);
    capture_autostart       = get_bool(kv, "capture_autostart", capture_autostart);

    multiview_layout        = clampi(get_int(kv, "multiview_layout", multiview_layout), 0, 15);
    multiview_preview       = get_bool(kv, "multiview_preview", multiview_preview);
    multiview_autostart     = get_bool(kv, "multiview_autostart", multiview_autostart);
    multiview_fullscreen    = get_bool(kv, "multiview_fullscreen", multiview_fullscreen);
    multiview_sources.clear();
    for (int i = 0; i < 32; ++i) {
        auto it = kv.find("multiview_source." + std::to_string(i));
        if (it == kv.end()) break;
        multiview_sources.push_back(it->second);
    }

    webcam_source           = get_str(kv, "webcam_source", webcam_source);
    webcam_width            = clampi(get_int(kv, "webcam_width", webcam_width), 160, 3840);
    webcam_height           = clampi(get_int(kv, "webcam_height", webcam_height), 120, 2160);
    webcam_fps              = clampi(get_int(kv, "webcam_fps", webcam_fps), 1, 120);
    webcam_autostart        = get_bool(kv, "webcam_autostart", webcam_autostart);

    identify_sources        = get_bool(kv, "identify_sources", identify_sources);
    check_updates_on_launch = get_bool(kv, "check_updates_on_launch", check_updates_on_launch);

    // Manual sources are stored as numbered keys so an address can contain
    // any character without needing a separator convention.
    manual_sources.clear();
    for (int i = 0; i < 256; ++i) {
        const std::string addr_key = "manual_source." + std::to_string(i);
        auto it = kv.find(addr_key);
        if (it == kv.end()) continue;
        if (it->second.empty()) continue;
        ManualSource ms;
        ms.address = it->second;
        auto name_it = kv.find("manual_name." + std::to_string(i));
        if (name_it != kv.end()) ms.name = name_it->second;
        manual_sources.push_back(std::move(ms));
    }

    sources_x               = get_int(kv, "sources_x", sources_x);
    sources_y               = get_int(kv, "sources_y", sources_y);
}

void Settings::save() const {
    std::string out;
    out.reserve(2048);
    auto putb = [&](const char* k, bool v) { out += k; out += v ? "=1\r\n" : "=0\r\n"; };
    auto puti = [&](const char* k, int v) {
        char b[64]; snprintf(b, sizeof(b), "%s=%d\r\n", k, v); out += b;
    };
    auto puts_ = [&](const std::string& k, const std::string& v) {
        out += k; out += '='; out += v; out += "\r\n";
    };

    out += "# OMT Mini settings. Edited live by the app; hand edits are picked up on restart.\r\n";
    out += "\r\n# General\r\n";
    putb("run_at_login", run_at_login);
    putb("show_sources_on_start", show_sources_on_start);
    putb("notify_on_source_change", notify_on_source_change);
    putb("confirm_exit", confirm_exit);

    out += "\r\n# Network\r\n";
    puts_("discovery_server", discovery_server);
    puti("port_start", port_start);
    puti("port_end", port_end);

    out += "\r\n# Viewer\r\n";
    puti("viewer_quality", viewer_quality);
    puti("viewer_format", viewer_format);
    putb("viewer_audio", viewer_audio);
    puti("viewer_volume", viewer_volume);
    putb("viewer_show_stats", viewer_show_stats);
    putb("viewer_always_on_top", viewer_always_on_top);
    putb("viewer_low_bandwidth", viewer_low_bandwidth);
    puti("viewer_width", viewer_width);
    puti("viewer_height", viewer_height);

    out += "\r\n# Desktop capture\r\n";
    puts_("capture_name", capture_name);
    puti("capture_monitor", capture_monitor);
    puti("capture_fps", capture_fps);
    puti("capture_quality", capture_quality);
    putb("capture_cursor", capture_cursor);
    putb("capture_audio", capture_audio);
    putb("capture_autostart", capture_autostart);

    out += "\r\n# Multiview\r\n";
    puti("multiview_layout", multiview_layout);
    putb("multiview_preview", multiview_preview);
    putb("multiview_autostart", multiview_autostart);
    putb("multiview_fullscreen", multiview_fullscreen);
    for (size_t i = 0; i < multiview_sources.size(); ++i)
        puts_("multiview_source." + std::to_string(i), multiview_sources[i]);

    out += "\r\n# Webcam output\r\n";
    puts_("webcam_source", webcam_source);
    puti("webcam_width", webcam_width);
    puti("webcam_height", webcam_height);
    puti("webcam_fps", webcam_fps);
    putb("webcam_autostart", webcam_autostart);

    putb("identify_sources", identify_sources);

    out += "\r\n# Updates\r\n";
    putb("check_updates_on_launch", check_updates_on_launch);

    out += "\r\n# Manually added sources\r\n";
    for (size_t i = 0; i < manual_sources.size(); ++i) {
        const std::string index = std::to_string(i);
        puts_("manual_source." + index, manual_sources[i].address);
        if (!manual_sources[i].name.empty())
            puts_("manual_name." + index, manual_sources[i].name);
    }

    out += "\r\n# Window placement\r\n";
    puti("sources_x", sources_x);
    puti("sources_y", sources_y);

    // Write to a temp file and move into place so a crash mid-write cannot
    // leave a truncated config behind.
    const std::wstring final_path = ini_path();
    const std::wstring tmp_path   = final_path + L".tmp";

    HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(h);
    MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Settings::apply_to_libomt() const {
    if (!omt::loaded()) return;
    const auto& api = omt::api();
    api.settings_set_string("DiscoveryServer", discovery_server.c_str());
    api.settings_set_integer("NetworkPortStart", port_start);
    api.settings_set_integer("NetworkPortEnd", port_end);
    util::logf("omt: settings applied (discovery='%s' ports=%d-%d)",
               discovery_server.c_str(), port_start, port_end);
}
