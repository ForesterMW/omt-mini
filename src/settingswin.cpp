#include "settingswin.h"
#include "app.h"
#include "settings.h"
#include "discovery.h"
#include "multiview.h"
#include "announce.h"
#include "webui.h"
#include "capture.h"
#include "webcam.h"
#include "omt.h"
#include "update.h"
#include "discovery.h"
#include "multiview.h"
#include "announce.h"
#include "webui.h"

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

// Sized from its own initialiser, then checked against the enum, so a tab
// added to one and not the other will not build.
constexpr const wchar_t* kTabLabels[] = {
    L"General", L"Sources", L"Network", L"Viewer", L"Desktop", L"Webcam",
    L"Multiview", L"Web", L"About",
};
static_assert(sizeof(kTabLabels) / sizeof(kTabLabels[0]) == settings_tab::Count,
              "every settings tab needs a label, and every label needs a tab");

const std::vector<std::wstring>& tab_labels() {
    static const std::vector<std::wstring> v(std::begin(kTabLabels), std::end(kTabLabels));
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
    manual_editing_.clear();
    manual_address_text_.clear();
    manual_name_text_.clear();
    discovery_text_    = util::widen(cfg.discovery_server);
    port_start_text_   = std::to_wstring(cfg.port_start);
    port_end_text_     = std::to_wstring(cfg.port_end);
    capture_name_text_ = util::widen(cfg.capture_name);
    mv_name_text_      = util::widen(cfg.multiview_output_name);
    web_port_text_     = std::to_wstring(cfg.web_port);
    monitors_          = DesktopCapture::enumerate_monitors();

    if (hwnd_) {
        if (!visible()) show();
        else { ShowWindow(hwnd_, SW_RESTORE); SetForegroundWindow(hwnd_); }
        invalidate();
        return;
    }
    if (!create(L"OMT Mini Settings", 780, 600, true)) return;
    center_on_cursor();
    show();
}

bool SettingsWindow::on_close_request() {
    Settings& cfg = settings();
    cfg.capture_name = util::narrow(capture_name_text_);
    if (cfg.capture_name.empty()) cfg.capture_name = "Desktop";
    cfg.multiview_output_name = util::narrow(mv_name_text_);
    if (cfg.multiview_output_name.empty()) cfg.multiview_output_name = "Multiview";
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

    if (row(204, L"Identify sources",
            L"Briefly connects to each new source, asking for metadata only, to "
            L"read the product name it reports. Once per source.",
            &cfg.identify_sources))
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

// ---- Sources (hand added senders) --------------------------------------
void SettingsWindow::tab_sources(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    float y = area.top + 4.0f;

    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 52.0f),
                     L"Sources on the local network are found automatically. Add a "
                     L"sender here when discovery cannot reach it: a different "
                     L"subnet, the far end of a VPN, or a host where mDNS is "
                     L"blocked.",
                     Font::Small, theme().text_dim);
    y += 56.0f;

    // ---- add row ----
    ctx.text(ui::rect(area.left, y, 120.0f, 26.0f), L"Address", Font::Body, theme().text);
    if (ctx.text_field(901, D2D1::RectF(area.left + 120.0f, y, area.left + 360.0f, y + 28.0f),
                       &manual_address_text_, L"10.0.0.5  or  host:6400"))
        manual_error_.clear();

    ctx.text(ui::rect(area.left + 372.0f, y, 60.0f, 26.0f), L"Name", Font::Body,
             theme().text_dim);
    ctx.text_field(902, D2D1::RectF(area.left + 428.0f, y, area.right - 92.0f, y + 28.0f),
                   &manual_name_text_, L"optional");

    const bool editing = !manual_editing_.empty();
    const bool can_add = !util::trim(util::narrow(manual_address_text_)).empty();
    if (ctx.button(903, ui::rect(area.right - 82.0f, y, 82.0f, 28.0f),
                   editing ? L"Save" : L"Add", ButtonStyle::Primary, can_add) ||
        (can_add && ctx.input().key == VK_RETURN)) {
        const std::string normalized =
            omt::normalize_address(util::narrow(manual_address_text_));
        if (normalized.empty()) {
            manual_error_ = L"That address could not be understood.";
        } else {
            const bool clashes = std::any_of(
                cfg.manual_sources.begin(), cfg.manual_sources.end(),
                [&](const ManualSource& m) {
                    return m.address == normalized && m.address != manual_editing_;
                });
            if (clashes) {
                manual_error_ = L"That address is already in the list.";
            } else if (editing) {
                for (auto& entry : cfg.manual_sources) {
                    if (entry.address != manual_editing_) continue;
                    entry.address = normalized;
                    {
                        std::vector<std::string> taken;
                        for (const auto& other : cfg.manual_sources)
                            if (other.address != manual_editing_) taken.push_back(other.name);
                        entry.name = omt::safe_source_name(
                            util::narrow(manual_name_text_),
                            omt::default_direct_name(taken));
                    }
                    break;
                }
                cfg.save();
                discovery().set_manual_sources(cfg.manual_sources);
                manual_editing_.clear();
                manual_address_text_.clear();
                manual_name_text_.clear();
                manual_error_.clear();
            } else {
                std::vector<std::string> taken;
                for (const auto& other : cfg.manual_sources) taken.push_back(other.name);

                ManualSource ms;
                ms.address = normalized;
                ms.name    = omt::safe_source_name(util::narrow(manual_name_text_),
                                                   omt::default_direct_name(taken));
                cfg.manual_sources.push_back(std::move(ms));
                cfg.save();
                discovery().set_manual_sources(cfg.manual_sources);
                manual_address_text_.clear();
                manual_name_text_.clear();
                manual_error_.clear();
            }
        }
        invalidate();
    }
    y += 32.0f;

    if (!manual_error_.empty()) {
        ctx.text(ui::rect(area.left + 120.0f, y, area.right - area.left - 120.0f, 18.0f),
                 manual_error_, Font::Small, theme().danger, Align::Left, false);
    } else {
        ctx.text(ui::rect(area.left + 120.0f, y, area.right - area.left - 120.0f, 18.0f),
                 editing ? L"Editing an existing source."
                         : L"Port 6400 is assumed when none is given.",
                 Font::Small, theme().text_dim, Align::Left, false);
    }
    y += 26.0f;

    ctx.separator(area.left, area.right, y);
    y += 10.0f;

    if (ctx.checkbox(904, ui::rect(area.left, y, 460.0f, 24.0f),
                     &cfg.announce_manual_sources,
                     L"Announce direct sources over mDNS")) {
        dirty_ = true;
        announcer().refresh();
    }
    y += 32.0f;

    // ---- existing entries ----
    if (cfg.manual_sources.empty()) {
        ctx.text(D2D1::RectF(area.left, y + 20.0f, area.right, y + 44.0f),
                 L"Nothing added yet", Font::Body, theme().text_dim, Align::Center);
        return;
    }

    const auto live_sources = discovery().sources();

    const float row_h = 44.0f;
    const D2D1_RECT_F list = D2D1::RectF(area.left, y, area.right, area.bottom);
    // Three widgets a row now, so the stride has to leave room for them.
    scroll_.content_height = cfg.manual_sources.size() * row_h + 6.0f;
    ctx.begin_scroll(920, list, &scroll_);

    int remove_index = -1;
    float ry = list.top + 3.0f;
    for (size_t i = 0; i < cfg.manual_sources.size(); ++i) {
        const auto& m = cfg.manual_sources[i];
        const D2D1_RECT_F rowr = D2D1::RectF(list.left, ry, list.right - 8.0f,
                                             ry + row_h - 6.0f);
        const auto id = static_cast<ui::Id>(930 + i * 4);

        ctx.fill_rect(rowr, theme().panel, ui::metric::kRadius);

        // Same reachability the source list shows, so this page does not have
        // to be cross referenced with that one.
        SourceStatus status = SourceStatus::Unknown;
        for (const auto& live : live_sources) {
            if (live.address == m.address) { status = live.status; break; }
        }
        const D2D1_COLOR_F dot = status == SourceStatus::Online  ? theme().ok
                               : status == SourceStatus::Offline ? theme().danger
                                                                 : theme().text_dim;
        ctx.status_dot(rowr.left + 16.0f, rowr.top + 12.0f, 3.5f, dot);

        const float text_left = rowr.left + 30.0f;
        const std::wstring label = m.name.empty() ? util::widen(m.address)
                                                  : util::widen(m.name);
        ctx.text(ui::rect(text_left, rowr.top + 4.0f, 300.0f, 18.0f), label,
                 Font::BodyBold, theme().text, Align::Left, false);

        std::wstring detail = m.name.empty() ? std::wstring() : util::widen(m.address);
        if (status == SourceStatus::Offline)
            detail += detail.empty() ? L"not answering" : L"   not answering";
        ctx.text(ui::rect(text_left, rowr.top + 20.0f, 300.0f, 16.0f),
                 detail, Font::Small, theme().text_dim, Align::Left, false);

        if (ctx.button(id, ui::rect(rowr.right - 90.0f, rowr.top + 5.0f, 78.0f, 26.0f),
                       L"Remove", ButtonStyle::Normal))
            remove_index = static_cast<int>(i);

        if (!cfg.announce_manual_sources) {
            // With the blanket setting off, each source carries its own.
            const bool on = m.announce;
            if (ctx.button(id + 2,
                           ui::rect(rowr.right - 268.0f, rowr.top + 5.0f, 100.0f, 26.0f),
                           on ? L"Announced" : L"Announce",
                           on ? ButtonStyle::Primary : ButtonStyle::Normal)) {
                cfg.manual_sources[i].announce = !on;
                cfg.save();
                announcer().refresh();
                invalidate();
            }
        } else if (announcer().is_announced(m.address)) {
            ctx.badge(rowr.right - 250.0f, rowr.top + 9.0f, 18.0f, L"mDNS",
                      theme().ok);
        }

        if (ctx.button(id + 1, ui::rect(rowr.right - 160.0f, rowr.top + 5.0f, 62.0f, 26.0f),
                       L"Edit", ButtonStyle::Normal)) {
            manual_editing_      = m.address;
            manual_address_text_ = util::widen(m.address);
            manual_name_text_    = util::widen(m.name);
            manual_error_.clear();
            invalidate();
        }

        ry += row_h;
    }
    ctx.end_scroll();

    if (remove_index >= 0) {
        if (cfg.manual_sources[remove_index].address == manual_editing_) {
            manual_editing_.clear();
            manual_address_text_.clear();
            manual_name_text_.clear();
        }
        cfg.manual_sources.erase(cfg.manual_sources.begin() + remove_index);
        cfg.save();
        discovery().set_manual_sources(cfg.manual_sources);
        invalidate();
    }
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

// ---- Multiview ---------------------------------------------------------
void SettingsWindow::tab_multiview(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    const bool sending = multiview_output().running();
    float y = area.top + 4.0f;

    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 40.0f),
                     L"The multiview can be sent to other machines as a single OMT "
                     L"source. It does not need the window open to do it.",
                     Font::Small, theme().text_dim);
    y += 46.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Source name", Font::Body, theme().text);
    if (ctx.text_field(801, D2D1::RectF(area.left + kLabelW, y, area.right, y + 28.0f),
                       &mv_name_text_, L"Multiview"))
        dirty_ = true;
    y += 42.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Resolution", Font::Body, theme().text);
    static const std::vector<std::wstring> sizes = {
        L"1920 x 1080", L"1600 x 900", L"1280 x 720", L"960 x 540"
    };
    static const int size_w[4] = { 1920, 1600, 1280, 960 };
    static const int size_h[4] = { 1080,  900,  720,  540 };
    int size_index = 0;
    for (int i = 0; i < 4; ++i)
        if (size_w[i] == cfg.multiview_output_width) size_index = i;
    if (ctx.dropdown(802, D2D1::RectF(area.left + kLabelW, y, area.left + kLabelW + 160.0f,
                                      y + 28.0f), sizes, &size_index)) {
        cfg.multiview_output_width  = size_w[size_index];
        cfg.multiview_output_height = size_h[size_index];
        dirty_ = true;
    }

    ctx.text(ui::rect(area.left + kLabelW + 176.0f, y, 40.0f, 26.0f), L"fps", Font::Body,
             theme().text_dim);
    int fps_index = index_of(kFpsValues, 6, cfg.multiview_output_fps, 2);
    if (ctx.dropdown(803, D2D1::RectF(area.left + kLabelW + 216.0f, y,
                                      area.left + kLabelW + 306.0f, y + 28.0f),
                     fps_items(), &fps_index)) {
        cfg.multiview_output_fps = kFpsValues[fps_index];
        dirty_ = true;
    }
    y += 40.0f;

    if (sending) {
        ctx.text(ui::rect(area.left, y, area.right - area.left, 18.0f),
                 L"Changes apply the next time the output starts.", Font::Small,
                 theme().warn, Align::Left, false);
    }
    y += 26.0f;

    ctx.separator(area.left, area.right, y);
    y += 14.0f;

    if (ctx.checkbox(804, ui::rect(area.left, y, 420.0f, 24.0f),
                     &cfg.multiview_output_enabled,
                     L"Start sending the multiview when OMT Mini launches"))
        dirty_ = true;
    y += 30.0f;

    if (ctx.checkbox(805, ui::rect(area.left, y, 420.0f, 24.0f), &cfg.multiview_autostart,
                     L"Also open the multiview window at launch"))
        dirty_ = true;
    y += 28.0f;

    ctx.text_wrapped(D2D1::RectF(area.left + 28.0f, y, area.right, y + 34.0f),
                     L"Leave this off to run headless: the wall is built and sent, with "
                     L"nothing on screen on this machine.",
                     Font::Small, theme().text_dim);
    y += 42.0f;

    if (ctx.button(806, ui::rect(area.left, y, 160.0f, 32.0f),
                   sending ? L"Stop sending" : L"Start sending",
                   sending ? ButtonStyle::Danger : ButtonStyle::Primary)) {
        cfg.multiview_output_name = util::narrow(mv_name_text_);
        if (cfg.multiview_output_name.empty()) cfg.multiview_output_name = "Multiview";
        cfg.save();
        App::instance().toggle_multiview_output();
        invalidate();
    }

    if (ctx.button(807, ui::rect(area.left + 172.0f, y, 150.0f, 32.0f), L"Open the wall",
                   ButtonStyle::Normal))
        App::instance().show_multiview();

    const auto out = multiview_output().stats();
    if (out.running && out.fps > 0.0f) {
        ctx.text(ui::rect(area.left + 334.0f, y, area.right - area.left - 334.0f, 32.0f),
                 fmt(L"Live, %d receiver%s", out.connections,
                     out.connections == 1 ? L"" : L"s"),
                 Font::Small, theme().ok);
        ctx.request_redraw();
    }
}

// ---- Control panel -----------------------------------------------------
void SettingsWindow::tab_web(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    float y = area.top + 4.0f;

    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 40.0f),
                     L"A page for controlling this copy from a browser on another "
                     L"machine: what is open, where it is, and what it is showing.",
                     Font::Small, theme().text_dim);
    y += 46.0f;

    if (ctx.checkbox(1001, ui::rect(area.left, y, 420.0f, 24.0f), &cfg.web_enabled,
                     L"Serve the control panel")) {
        dirty_ = true;
        if (cfg.web_enabled) {
            if (web_server().start(cfg.web_port, App::instance().message_window()))
                App::instance().refresh_web_snapshot();
            else
                status_message_ = util::widen(web_server().error());
        } else {
            web_server().stop();
        }
        invalidate();
    }
    y += 34.0f;

    ctx.text(ui::rect(area.left, y, kLabelW, 26.0f), L"Port", Font::Body, theme().text);
    if (ctx.text_field(1002, D2D1::RectF(area.left + kLabelW, y,
                                         area.left + kLabelW + 110.0f, y + 28.0f),
                       &web_port_text_, L"7400", true))
        dirty_ = true;

    if (ctx.button(1003, ui::rect(area.left + kLabelW + 122.0f, y, 90.0f, 28.0f),
                   L"Apply", ButtonStyle::Normal)) {
        try { cfg.web_port = std::stoi(util::narrow(web_port_text_)); } catch (...) {}
        cfg.web_port = std::clamp(cfg.web_port, 1024, 65535);
        web_port_text_ = std::to_wstring(cfg.web_port);
        cfg.save();
        if (cfg.web_enabled) {
            web_server().stop();
            if (web_server().start(cfg.web_port, App::instance().message_window())) {
                App::instance().refresh_web_snapshot();
                status_message_ = L"Listening on port " + web_port_text_;
            } else {
                status_message_ = util::widen(web_server().error());
            }
            status_until_ms_ = util::now_ms() + 4000;
        }
        invalidate();
    }
    y += 42.0f;

    ctx.separator(area.left, area.right, y);
    y += 14.0f;

    if (web_server().running()) {
        ctx.text(ui::rect(area.left, y, 200.0f, 20.0f), L"Open from a browser at",
                 Font::Small, theme().text_dim, Align::Left, false);
        y += 22.0f;
        for (const auto& url : web_server().urls()) {
            ctx.text(ui::rect(area.left, y, area.right - area.left - 120.0f, 22.0f),
                     util::widen(url), Font::Mono, theme().ok, Align::Left, false);
            y += 24.0f;
        }
        y += 6.0f;
        if (ctx.button(1004, ui::rect(area.left, y, 170.0f, 32.0f), L"Open here",
                       ButtonStyle::Normal)) {
            const auto urls = web_server().urls();
            if (!urls.empty())
                ShellExecuteW(nullptr, L"open", util::widen(urls.back()).c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
        }
        y += 44.0f;
    } else if (!web_server().error().empty()) {
        ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 36.0f),
                         util::widen(web_server().error()), Font::Small, theme().danger);
        y += 40.0f;
    } else {
        ctx.text(ui::rect(area.left, y, area.right - area.left, 20.0f), L"Not running",
                 Font::Small, theme().text_dim, Align::Left, false);
        y += 30.0f;
    }

    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 52.0f),
                     L"Anyone who can reach this port can move, retarget and close "
                     L"windows on this machine. There is no password. Only turn it on "
                     L"where you trust the network.",
                     Font::Small, theme().warn);
}

// ---- About and updates -------------------------------------------------
void SettingsWindow::tab_about(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    Settings& cfg = settings();
    float y = area.top + 4.0f;

    ctx.text(ui::rect(area.left, y, 400.0f, 28.0f), L"OMT Mini", Font::Title, theme().text);
    y += 28.0f;
    ctx.text(ui::rect(area.left, y, 400.0f, 20.0f),
             L"Version " OMTMINI_VERSION_W, Font::Small, theme().text_dim);
    y += 28.0f;

    ctx.separator(area.left, area.right, y);
    y += 14.0f;

    // ---- updates ----
    const UpdateInfo up = updater().info();
    const bool working = up.state == UpdateState::Checking ||
                         up.state == UpdateState::Downloading;

    ctx.text(ui::rect(area.left, y, 300.0f, 20.0f), L"Updates", Font::BodyBold,
             theme().text, Align::Left, false);

    std::wstring status;
    D2D1_COLOR_F status_colour = theme().text_dim;
    switch (up.state) {
        case UpdateState::Checking:
            status = L"Checking GitHub...";
            break;
        case UpdateState::UpToDate:
            status = L"This is the latest stable release.";
            status_colour = theme().ok;
            break;
        case UpdateState::Available:
            status = fmt(L"Version %S is available.", up.latest_version.c_str());
            status_colour = theme().accent_hi;
            break;
        case UpdateState::Downloading:
            status = fmt(L"Downloading, %d%%", up.progress_percent);
            break;
        case UpdateState::ReadyToInstall:
            status = fmt(L"Version %S is ready to install.", up.latest_version.c_str());
            status_colour = theme().ok;
            break;
        case UpdateState::Failed:
            status = up.error.empty() ? L"The update check did not complete."
                                      : util::widen(up.error);
            status_colour = theme().warn;
            break;
        default:
            status = L"Checks the public GitHub releases. No account needed.";
            break;
    }
    ctx.text(ui::rect(area.left, y + 19.0f, area.right - area.left - 200.0f, 18.0f),
             status, Font::Small, status_colour, Align::Left, false);

    // The install step is always an explicit press. Nothing here restarts the
    // application on its own.
    if (up.state == UpdateState::ReadyToInstall) {
        if (ctx.button(710, ui::rect(area.right - 190.0f, y - 2.0f, 190.0f, 30.0f),
                       L"Install and restart", ButtonStyle::Primary)) {
            App::instance().install_update();
        }
    } else if (up.state == UpdateState::Available) {
        if (ctx.button(711, ui::rect(area.right - 190.0f, y - 2.0f, 190.0f, 30.0f),
                       fmt(L"Update to %S", up.latest_version.c_str()),
                       ButtonStyle::Primary)) {
            updater().download_and_install(App::instance().message_window());
        }
    } else {
        if (ctx.button(712, ui::rect(area.right - 190.0f, y - 2.0f, 190.0f, 30.0f),
                       working ? L"Working..." : L"Check for updates",
                       ButtonStyle::Normal, !working)) {
            updater().check(App::instance().message_window(), false);
        }
    }
    y += 44.0f;

    if (up.state == UpdateState::Downloading) {
        // Progress bar, so a slow connection does not look like a hang.
        const D2D1_RECT_F track = D2D1::RectF(area.left, y, area.right, y + 6.0f);
        ctx.fill_rect(track, theme().panel_hi, 3.0f);
        const float w = (area.right - area.left) * (up.progress_percent / 100.0f);
        ctx.fill_rect(D2D1::RectF(area.left, y, area.left + w, y + 6.0f),
                      theme().accent, 3.0f);
        y += 14.0f;
        ctx.request_redraw();
    }
    if (up.state == UpdateState::Checking) ctx.request_redraw();

    if ((up.state == UpdateState::Available || up.state == UpdateState::ReadyToInstall) &&
        !up.release_url.empty()) {
        if (ctx.button(713, ui::rect(area.left, y, 170.0f, 28.0f), L"Release notes",
                       ButtonStyle::Ghost)) {
            ShellExecuteW(nullptr, L"open", util::widen(up.release_url).c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        y += 34.0f;
    }

    if (ctx.checkbox(714, ui::rect(area.left, y, 400.0f, 24.0f),
                     &cfg.check_updates_on_launch,
                     L"Check for updates when OMT Mini starts"))
        dirty_ = true;
    y += 30.0f;
    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 34.0f),
                     L"Checking only reads the public release list. Installing always "
                     L"waits for you to press the button, so a machine on air is never "
                     L"restarted on its own.",
                     Font::Small, theme().text_dim);
    y += 40.0f;

    ctx.separator(area.left, area.right, y);
    y += 14.0f;

    auto info = [&](const wchar_t* k, const std::wstring& v) {
        ctx.text(ui::rect(area.left, y, 150.0f, 20.0f), k, Font::Small, theme().text_dim);
        ctx.text(ui::rect(area.left + 150.0f, y, area.right - area.left - 150.0f, 20.0f),
                 v, Font::Small, theme().text);
        y += 24.0f;
    };
    info(L"libomt", omt::loaded() ? L"loaded" : L"not loaded");
    info(L"Renderer", gfx::Device::is_warp() ? L"Direct3D 11 (WARP software)"
                                             : L"Direct3D 11");
    y += 8.0f;

    if (ctx.button(701, ui::rect(area.left, y, 150.0f, 30.0f), L"Open log folder",
                   ButtonStyle::Normal))
        ShellExecuteW(nullptr, L"open", util::config_dir().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);

    if (ctx.button(702, ui::rect(area.left + 162.0f, y, 150.0f, 30.0f), L"Project page",
                   ButtonStyle::Normal))
        ShellExecuteW(nullptr, L"open", L"https://github.com/ForesterMW/omt-mini",
                      nullptr, nullptr, SW_SHOWNORMAL);
    y += 38.0f;

    ctx.text_wrapped(D2D1::RectF(area.left, y, area.right, y + 40.0f),
                     L"Not affiliated with vMix or the Open Media Transport project. "
                     L"libomt and libvmx are MIT licensed and redistributed unmodified.",
                     Font::Small, theme().text_dim);
}

void SettingsWindow::on_render(ui::Ctx& ctx) {
    ctx.fill_rect(D2D1::RectF(0, 0, ctx.width(), ctx.height()), theme().bg);

    const D2D1_RECT_F tabs = D2D1::RectF(16.0f, 8.0f, ctx.width() - 16.0f, 8.0f + kTabsH);
    // tabs() claims base+1 through base+count, so this base is kept clear of
    // every other id in this window.
    if (ctx.tabs(2000, tabs, tab_labels(), &active_tab_)) invalidate();

    const float footer_h = 48.0f;
    const D2D1_RECT_F body = D2D1::RectF(24.0f, tabs.bottom + 16.0f,
                                         ctx.width() - 24.0f,
                                         ctx.height() - footer_h);

    switch (active_tab_) {
        case settings_tab::General:   tab_general(ctx, body);   break;
        case settings_tab::Sources:   tab_sources(ctx, body);   break;
        case settings_tab::Network:   tab_network(ctx, body);   break;
        case settings_tab::Viewer:    tab_viewer(ctx, body);    break;
        case settings_tab::Desktop:   tab_desktop(ctx, body);   break;
        case settings_tab::Webcam:    tab_webcam(ctx, body);    break;
        case settings_tab::Multiview: tab_multiview(ctx, body); break;
        case settings_tab::Web:       tab_web(ctx, body);       break;
        default:                      tab_about(ctx, body);     break;
    }

    // Footer: transient status on the left, close on the right.
    if (!status_message_.empty() && util::now_ms() < status_until_ms_) {
        ctx.text(ui::rect(24.0f, ctx.height() - footer_h, ctx.width() - 160.0f, footer_h),
                 status_message_, Font::Small, theme().text_dim);
        ctx.request_redraw();
    }

    if (ctx.button(2500, ui::rect(ctx.width() - 116.0f, ctx.height() - footer_h + 6.0f,
                                 92.0f, 30.0f), L"Close", ButtonStyle::Normal)) {
        on_close_request();
    }

    if (dirty_) {
        settings().save();
        dirty_ = false;
    }
}
