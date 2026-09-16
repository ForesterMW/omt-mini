#include "settingswin.h"
#include "app.h"
#include "settings.h"
#include "discovery.h"
#include "capture.h"
#include "webcam.h"
#include "omt.h"

#include <shellapi.h>
#include <algorithm>
#include <cstdio>

using gfx::theme;
using ui::Align;
using ui::ButtonStyle;
using ui::Font;

namespace {
constexpr float kTabsH  = 44.0f;
constexpr float kRowH   = 44.0f;
constexpr float kLabelW = 190.0f;

const std::vector<std::wstring>& tab_labels() {
    static const std::vector<std::wstring> v = {
        L"General", L"Network", L"Viewer", L"Desktop", L"Webcam", L"About"
    };
    return v;
}

// Only the formats the renderer can actually display are offered.
const std::vector<std::wstring>& format_items() {
    static const std::vector<std::wstring> v = {
        L"UYVY (fastest)", L"UYVY or BGRA (alpha aware)", L"BGRA (always RGB)",
        L"UYVY or UYVA (alpha aware)"
    };
    return v;
}
const int kFormatValues[4] = {
    OMTPreferredVideoFormat_UYVY,
    OMTPreferredVideoFormat_UYVYorBGRA,
    OMTPreferredVideoFormat_BGRA,
    OMTPreferredVideoFormat_UYVYorUYVA,
};

const std::vector<std::wstring>& quality_items() {
    static const std::vector<std::wstring> v = { L"Auto", L"Low", L"Medium", L"High" };
    return v;
}
const int kQualityValues[4] = {
    OMTQuality_Default, OMTQuality_Low, OMTQuality_Medium, OMTQuality_High
};

const std::vector<std::wstring>& fps_items() {
    static const std::vector<std::wstring> v = { L"15", L"24", L"25", L"30", L"50", L"60" };
    return v;
}
const int kFpsValues[6] = { 15, 24, 25, 30, 50, 60 };

// Must stay in step with kFormats in vcam/vcam.cpp.
const std::vector<std::wstring>& resolution_items() {
    static const std::vector<std::wstring> v = {
        L"1920 x 1080", L"1280 x 720", L"960 x 540", L"640 x 360", L"640 x 480"
    };
    return v;
}
const int kResWidths[5]  = { 1920, 1280, 960, 640, 640 };
const int kResHeights[5] = { 1080,  720, 540, 360, 480 };

int index_of(const int* values, int count, int value, int fallback) {
    for (int i = 0; i < count; ++i)
        if (values[i] == value) return i;
    return fallback;
}

std::wstring fmt(const wchar_t* format, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf(buf, 512, format, args);
    va_end(args);
    buf[511] = L'\0';
    return buf;
}
} // namespace

void SettingsWindow::open_or_focus(int tab) {
    active_tab_ = std::clamp(tab, 0, static_cast<int>(tab_labels().size()) - 1);

    const Settings& cfg = settings();
    discovery_text_    = util::widen(cfg.discovery_server);
    port_start_text_   = std::to_wstring(cfg.port_start);
    port_end_text_     = std::to_wstring(cfg.port_end);
    capture_name_text_ = util::widen(cfg.capture_name);
    monitors_          = DesktopCapture::enumerate_monitors();

    if (hwnd_) {
        if (!visible()) show();
        else { ShowWindow(hwnd_, SW_RESTORE); SetForegroundWindow(hwnd_); }
        invalidate();
        return;
    }
    if (!create(L"OMT Mini Settings", 620, 560, true)) return;
    center_on_cursor();
    show();
}

bool SettingsWindow::on_close_request() {
    Settings& cfg = settings();
    cfg.capture_name = util::narrow(capture_name_text_);
    if (cfg.capture_name.empty()) cfg.capture_name = "Desktop";
    cfg.save();
    hide();
    return false;
}

// ---- General -----------------------------------------------------------
void SettingsWindow::tab_general(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    float y = area.top + 8.0f;

    auto row = [&](ui::Id id, const wchar_t* label, const wchar_t* help, bool* value) {
        ctx.text(ui::rect(area.left, y, kLabelW + 180.0f, 20.0f), label, Font::Body,
                 theme().text, Align::Left, false);
        ctx.text(ui::rect(area.left, y + 19.0f, area.right - area.left - 70.0f, 18.0f),
                 help, Font::Small, theme().text_dim, Align::Left, false);
        const bool changed =
            ctx.toggle(id, D2D1::RectF(area.right - 60.0f, y, area.right, y + 22.0f),
                       value, L"");
        y += kRowH + 12.0f;
        return changed;
    };

    bool login = App::run_at_login();
    if (row(201, L"Start with Windows",
            L"Runs OMT Mini in the tray when you sign in.", &login)) {
        App::set_run_at_login(login);
        cfg.run_at_login = login;
        dirty_ = true;
    }

    if (row(202, L"Open the source list on launch",
            L"Otherwise OMT Mini starts silently in the tray.",
            &cfg.show_sources_on_start))
        dirty_ = true;

    if (row(203, L"Notify when sources change",
            L"Shows a tray balloon as sources appear and disappear.",
            &cfg.notify_on_source_change))
        dirty_ = true;

    ctx.separator(area.left, area.right, y);
    y += 16.0f;

    ctx.text(ui::rect(area.left, y, 300.0f, 20.0f), L"Viewers open", Font::Body,
             theme().text, Align::Left, false);
    ctx.text(ui::rect(area.left, y + 19.0f, 300.0f, 18.0f),
             fmt(L"%zu right now", App::instance().viewer_count()),
             Font::Small, theme().text_dim, Align::Left, false);
    if (ctx.button(205, ui::rect(area.right - 120.0f, y, 120.0f, 28.0f),
                   L"Close all", ButtonStyle::Normal))
        App::instance().close_all_viewers();
}

// ---- Network -----------------------------------------------------------
void SettingsWindow::tab_network(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    float y = area.top + 8.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 20.0f), L"Discovery server", Font::Body,
             theme().text, Align::Left, false);
    if (ctx.text_field(301, D2D1::RectF(area.left + kLabelW, y - 4.0f, area.right, y + 28.0f),
                       &discovery_text_, L"omt://hostname:port"))
        dirty_ = true;
    y += 30.0f;
    ctx.text(ui::rect(area.left, y, area.right - area.left, 18.0f),
             L"Leave blank to use standard DNS-SD discovery on the local network.",
             Font::Small, theme().text_dim, Align::Left, false);
    y += 34.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 20.0f), L"Sender port range", Font::Body,
             theme().text, Align::Left, false);
    if (ctx.text_field(302, D2D1::RectF(area.left + kLabelW, y - 4.0f,
                                        area.left + kLabelW + 92.0f, y + 28.0f),
                       &port_start_text_, L"6400", true))
        dirty_ = true;
    ctx.text(ui::rect(area.left + kLabelW + 100.0f, y, 20.0f, 20.0f), L"to", Font::Body,
             theme().text_dim, Align::Center, false);
    if (ctx.text_field(303, D2D1::RectF(area.left + kLabelW + 126.0f, y - 4.0f,
                                        area.left + kLabelW + 218.0f, y + 28.0f),
                       &port_end_text_, L"6600", true))
        dirty_ = true;
    y += 30.0f;
    ctx.text(ui::rect(area.left, y, area.right - area.left, 18.0f),
             L"Ports OMT Mini's own senders bind to. Open these on the firewall.",
             Font::Small, theme().text_dim, Align::Left, false);
    y += 40.0f;

    if (ctx.button(304, ui::rect(area.left, y, 150.0f, 32.0f), L"Apply now",
                   ButtonStyle::Primary)) {
        cfg.discovery_server = util::narrow(discovery_text_);
        try { cfg.port_start = std::stoi(util::narrow(port_start_text_)); } catch (...) {}
        try { cfg.port_end   = std::stoi(util::narrow(port_end_text_)); } catch (...) {}
        cfg.port_start = std::clamp(cfg.port_start, 1024, 65535);
        cfg.port_end   = std::clamp(cfg.port_end, cfg.port_start, 65535);
        port_start_text_ = std::to_wstring(cfg.port_start);
        port_end_text_   = std::to_wstring(cfg.port_end);
        cfg.apply_to_libomt();
        cfg.save();
        discovery().refresh_now();
        status_message_  = L"Applied. Existing senders keep their current ports.";
        status_until_ms_ = util::now_ms() + 4000;
    }

    if (ctx.button(305, ui::rect(area.left + 162.0f, y, 150.0f, 32.0f),
                   L"Rescan network", ButtonStyle::Normal)) {
        discovery().refresh_now();
        status_message_  = L"Rescanning.";
        status_until_ms_ = util::now_ms() + 2000;
    }
    y += 44.0f;

    ctx.text(ui::rect(area.left, y, area.right - area.left, 18.0f),
             fmt(L"%zu source%s visible right now", discovery().count(),
                 discovery().count() == 1 ? L"" : L"s"),
             Font::Small, theme().text_dim, Align::Left, false);
}

// ---- Viewer ------------------------------------------------------------
void SettingsWindow::tab_viewer(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    float y = area.top + 8.0f;

    auto label = [&](const wchar_t* text) {
        ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), text, Font::Body, theme().text);
    };

    label(L"Requested quality");
    int q = index_of(kQualityValues, 4, cfg.viewer_quality, 0);
    if (ctx.dropdown(401, D2D1::RectF(area.left + kLabelW, y, area.left + kLabelW + 200.0f,
                                      y + 28.0f), quality_items(), &q)) {
        cfg.viewer_quality = kQualityValues[q];
        dirty_ = true;
    }
    y += 42.0f;

    label(L"Preferred format");
    int f = index_of(kFormatValues, 4, cfg.viewer_format, 1);
    if (ctx.dropdown(402, D2D1::RectF(area.left + kLabelW, y, area.right, y + 28.0f),
                     format_items(), &f)) {
        cfg.viewer_format = kFormatValues[f];
        dirty_ = true;
    }
    y += 32.0f;
    ctx.text(ui::rect(area.left + kLabelW, y, area.right - area.left - kLabelW, 18.0f),
             L"Applies to viewers opened from now on.", Font::Small, theme().text_dim,
             Align::Left, false);
    y += 32.0f;

    label(L"Default volume");
    float vol = cfg.viewer_volume / 100.0f;
    if (ctx.slider(403, D2D1::RectF(area.left + kLabelW, y + 4.0f,
                                    area.left + kLabelW + 200.0f, y + 24.0f),
                   &vol, 0.0f, 1.0f)) {
        cfg.viewer_volume = static_cast<int>(vol * 100.0f + 0.5f);
        dirty_ = true;
    }
    ctx.text(ui::rect(area.left + kLabelW + 212.0f, y, 60.0f, 26.0f),
             fmt(L"%d%%", cfg.viewer_volume), Font::Small, theme().text_dim);
    y += 44.0f;

    auto check = [&](ui::Id id, const wchar_t* text, bool* value) {
        if (ctx.checkbox(id, ui::rect(area.left, y, 330.0f, 24.0f), value, text))
            dirty_ = true;
        y += 32.0f;
    };
    check(404, L"Monitor audio by default", &cfg.viewer_audio);
    check(405, L"Show the statistics panel by default", &cfg.viewer_show_stats);
    check(406, L"Keep viewers above other windows", &cfg.viewer_always_on_top);
    check(407, L"Low bandwidth preview (one eighth resolution)", &cfg.viewer_low_bandwidth);
}

// ---- Desktop capture ---------------------------------------------------
void SettingsWindow::tab_desktop(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    const bool live = desktop_capture().running();
    float y = area.top + 8.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Source name", Font::Body, theme().text);
    if (ctx.text_field(501, D2D1::RectF(area.left + kLabelW, y, area.right, y + 28.0f),
                       &capture_name_text_, L"Desktop")) {
        dirty_ = true;
    }
    y += 42.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Monitor", Font::Body, theme().text);
    std::vector<std::wstring> monitor_names;
    for (const auto& m : monitors_) monitor_names.push_back(m.name);
    if (monitor_names.empty()) monitor_names.push_back(L"No monitors detected");
    int mon = std::clamp(cfg.capture_monitor, 0, static_cast<int>(monitor_names.size()) - 1);
    if (ctx.dropdown(502, D2D1::RectF(area.left + kLabelW, y, area.right, y + 28.0f),
                     monitor_names, &mon)) {
        cfg.capture_monitor = mon;
        dirty_ = true;
    }
    y += 42.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Frame rate", Font::Body, theme().text);
    int fi = index_of(kFpsValues, 6, cfg.capture_fps, 3);
    if (ctx.dropdown(503, D2D1::RectF(area.left + kLabelW, y, area.left + kLabelW + 110.0f,
                                      y + 28.0f), fps_items(), &fi)) {
        cfg.capture_fps = kFpsValues[fi];
        dirty_ = true;
    }

    int cq = index_of(kQualityValues, 4, cfg.capture_quality, 0);
    ctx.text(ui::rect(area.left + kLabelW + 126.0f, y, 60.0f, 26.0f), L"Quality",
             Font::Body, theme().text_dim);
    if (ctx.dropdown(504, D2D1::RectF(area.left + kLabelW + 190.0f, y, area.right, y + 28.0f),
                     quality_items(), &cq)) {
        cfg.capture_quality = kQualityValues[cq];
        dirty_ = true;
    }
    y += 44.0f;

    auto check = [&](ui::Id id, const wchar_t* text, bool* value) {
        if (ctx.checkbox(id, ui::rect(area.left, y, 380.0f, 24.0f), value, text))
            dirty_ = true;
        y += 30.0f;
    };
    check(505, L"Include the mouse cursor", &cfg.capture_cursor);
    check(506, L"Include system audio (loopback)", &cfg.capture_audio);
    check(507, L"Start capturing when OMT Mini launches", &cfg.capture_autostart);
    y += 10.0f;

    if (live) {
        ctx.text(ui::rect(area.left, y, area.right - area.left, 18.0f),
                 L"Changes apply the next time capture starts.", Font::Small,
                 theme().warn, Align::Left, false);
    }
    y += 24.0f;

    if (ctx.button(508, ui::rect(area.left, y, 150.0f, 32.0f),
                   live ? L"Stop capture" : L"Start capture",
                   live ? ButtonStyle::Danger : ButtonStyle::Primary)) {
        cfg.capture_name = util::narrow(capture_name_text_);
        if (cfg.capture_name.empty()) cfg.capture_name = "Desktop";
        cfg.save();
        App::instance().toggle_desktop_capture();
        invalidate();
    }

    const auto st = desktop_capture().stats();
    if (st.running && !st.address.empty()) {
        ctx.text(ui::rect(area.left + 164.0f, y, area.right - area.left - 164.0f, 32.0f),
                 fmt(L"Live as  %S", st.address.c_str()), Font::Small, theme().ok);
    } else if (!st.error.empty()) {
        ctx.text(ui::rect(area.left + 164.0f, y, area.right - area.left - 164.0f, 32.0f),
                 util::widen(st.error), Font::Small, theme().danger);
    }
    if (live) ctx.request_redraw();
}

// ---- Webcam ------------------------------------------------------------
void SettingsWindow::tab_webcam(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    const bool live = webcam().running();
    float y = area.top + 8.0f;

    // Source list, plus whatever is configured even if it is offline now.
    const auto sources = discovery().sources();
    std::vector<std::wstring> items;
    std::vector<std::string>  addresses;
    items.push_back(L"None");
    addresses.push_back("");
    int selected = 0;
    for (const auto& s : sources) {
        addresses.push_back(s.address);
        items.push_back(util::widen(s.name) + L"   (" + util::widen(s.host) + L")");
        if (s.address == cfg.webcam_source) selected = static_cast<int>(items.size()) - 1;
    }
    if (selected == 0 && !cfg.webcam_source.empty()) {
        addresses.push_back(cfg.webcam_source);
        items.push_back(util::widen(omt::short_name(cfg.webcam_source)) + L"   (offline)");
        selected = static_cast<int>(items.size()) - 1;
    }

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Source", Font::Body, theme().text);
    if (ctx.dropdown(601, D2D1::RectF(area.left + kLabelW, y, area.right, y + 28.0f),
                     items, &selected)) {
        cfg.webcam_source = addresses[std::clamp(selected, 0,
                                                 static_cast<int>(addresses.size()) - 1)];
        cfg.save();
        if (live) {
            webcam().stop();
            if (!cfg.webcam_source.empty()) webcam().start(cfg.webcam_source);
        }
    }
    y += 42.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Resolution", Font::Body, theme().text);
    int res = 1;
    for (int i = 0; i < 5; ++i)
        if (kResWidths[i] == cfg.webcam_width && kResHeights[i] == cfg.webcam_height) res = i;
    if (ctx.dropdown(602, D2D1::RectF(area.left + kLabelW, y, area.left + kLabelW + 160.0f,
                                      y + 28.0f), resolution_items(), &res)) {
        cfg.webcam_width  = kResWidths[res];
        cfg.webcam_height = kResHeights[res];
        dirty_ = true;
    }

    int wf = index_of(kFpsValues, 6, cfg.webcam_fps, 3);
    ctx.text(ui::rect(area.left + kLabelW + 176.0f, y, 40.0f, 26.0f), L"fps", Font::Body,
             theme().text_dim);
    if (ctx.dropdown(603, D2D1::RectF(area.left + kLabelW + 216.0f, y,
                                      area.left + kLabelW + 306.0f, y + 28.0f),
                     fps_items(), &wf)) {
        cfg.webcam_fps = kFpsValues[wf];
        dirty_ = true;
    }
    y += 44.0f;

    if (ctx.checkbox(604, ui::rect(area.left, y, 380.0f, 24.0f), &cfg.webcam_autostart,
                     L"Start the webcam output when OMT Mini launches"))
        dirty_ = true;
    y += 38.0f;

    ctx.separator(area.left, area.right, y);
    y += 16.0f;

    // ---- filter registration ----
    const bool registered = WebcamOutput::filter_registered();
    ctx.text(ui::rect(area.left, y, 320.0f, 20.0f), L"Virtual camera device", Font::Body,
             theme().text, Align::Left, false);
    ctx.text(ui::rect(area.left, y + 19.0f, area.right - area.left - 150.0f, 18.0f),
             registered ? L"Registered for this user. Pick \"OMT Mini Virtual Camera\" "
                          L"in other apps."
                        : L"Not registered yet. Other apps will not list the camera.",
             Font::Small, registered ? theme().ok : theme().text_dim, Align::Left, false);

    if (ctx.button(605, ui::rect(area.right - 140.0f, y, 140.0f, 30.0f),
                   registered ? L"Unregister" : L"Register",
                   registered ? ButtonStyle::Normal : ButtonStyle::Primary)) {
        std::wstring message;
        if (registered && live) webcam().stop();
        WebcamOutput::register_filter(!registered, &message);
        status_message_  = message;
        status_until_ms_ = util::now_ms() + 5000;
        invalidate();
    }
    y += 52.0f;

    if (ctx.button(606, ui::rect(area.left, y, 150.0f, 32.0f),
                   live ? L"Stop webcam" : L"Start webcam",
                   live ? ButtonStyle::Danger : ButtonStyle::Primary)) {
        cfg.save();
        App::instance().toggle_webcam();
        invalidate();
    }

    const auto st = webcam().stats();
    if (st.running) {
        ctx.text(ui::rect(area.left + 164.0f, y, area.right - area.left - 164.0f, 32.0f),
                 st.connected ? fmt(L"Publishing  %dx%d  %.0f fps", st.width, st.height, st.fps)
                              : std::wstring(L"Waiting for the source"),
                 Font::Small, st.connected ? theme().ok : theme().warn);
        ctx.request_redraw();
    }
}

// ---- About -------------------------------------------------------------
void SettingsWindow::tab_about(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    float y = area.top + 8.0f;

    ctx.text(ui::rect(area.left, y, 400.0f, 28.0f), L"OMT Mini", Font::Title, theme().text);
    y += 30.0f;
    ctx.text(ui::rect(area.left, y, 400.0f, 20.0f),
             L"Version " OMTMINI_VERSION_W, Font::Small, theme().text_dim);
    y += 30.0f;

    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 60.0f),
                     L"A single tray application for Open Media Transport: discover "
                     L"sources, open as many viewers as you like, share a screen, and "
                     L"appear as a webcam in other apps.",
                     Font::Body, theme().text_dim);
    y += 68.0f;

    ctx.separator(area.left, area.right, y);
    y += 16.0f;

    auto info = [&](const wchar_t* k, const std::wstring& v) {
        ctx.text(ui::rect(area.left, y, 150.0f, 20.0f), k, Font::Small, theme().text_dim);
        ctx.text(ui::rect(area.left + 150.0f, y, area.right - area.left - 150.0f, 20.0f),
                 v, Font::Small, theme().text);
        y += 24.0f;
    };
    info(L"libomt", omt::loaded() ? L"loaded" : L"not loaded");
    info(L"Renderer", gfx::Device::is_warp() ? L"Direct3D 11 (WARP software)"
                                             : L"Direct3D 11");
    info(L"Settings", util::config_dir());
    y += 12.0f;

    if (ctx.button(701, ui::rect(area.left, y, 160.0f, 32.0f), L"Open log folder",
                   ButtonStyle::Normal))
        ShellExecuteW(nullptr, L"open", util::config_dir().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);

    if (ctx.button(702, ui::rect(area.left + 172.0f, y, 200.0f, 32.0f),
                   L"Open Media Transport", ButtonStyle::Normal))
        ShellExecuteW(nullptr, L"open", L"https://openmediatransport.org", nullptr,
                      nullptr, SW_SHOWNORMAL);
    y += 44.0f;

    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 60.0f),
                     L"OMT Mini is not affiliated with vMix or the Open Media Transport "
                     L"project. libomt and libvmx are MIT licensed and are redistributed "
                     L"unmodified.",
                     Font::Small, theme().text_dim);
}

void SettingsWindow::on_render(ui::Ctx& ctx) {
    ctx.fill_rect(D2D1::RectF(0, 0, ctx.width(), ctx.height()), theme().bg);

    const D2D1_RECT_F tabs = D2D1::RectF(16.0f, 8.0f, ctx.width() - 16.0f, 8.0f + kTabsH);
    if (ctx.tabs(800, tabs, tab_labels(), &active_tab_)) invalidate();

    const float footer_h = 48.0f;
    const D2D1_RECT_F body = D2D1::RectF(24.0f, tabs.bottom + 16.0f,
                                         ctx.width() - 24.0f,
                                         ctx.height() - footer_h);

    switch (active_tab_) {
        case 0: tab_general(ctx, body); break;
        case 1: tab_network(ctx, body); break;
        case 2: tab_viewer(ctx, body);  break;
        case 3: tab_desktop(ctx, body); break;
        case 4: tab_webcam(ctx, body);  break;
        default: tab_about(ctx, body);  break;
    }

    // Footer: transient status on the left, close on the right.
    if (!status_message_.empty() && util::now_ms() < status_until_ms_) {
        ctx.text(ui::rect(24.0f, ctx.height() - footer_h, ctx.width() - 160.0f, footer_h),
                 status_message_, Font::Small, theme().text_dim);
        ctx.request_redraw();
    }

    if (ctx.button(801, ui::rect(ctx.width() - 116.0f, ctx.height() - footer_h + 6.0f,
                                 92.0f, 30.0f), L"Close", ButtonStyle::Normal)) {
        on_close_request();
    }

    if (dirty_) {
        settings().save();
        dirty_ = false;
    }
}
