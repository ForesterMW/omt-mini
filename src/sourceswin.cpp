#include "sourceswin.h"
#include "app.h"
#include "capture.h"
#include "webcam.h"
#include "settings.h"
#include "scan.h"
#include "multiview.h"
#include "announce.h"
#include "omt.h"
#include "update.h"
#include "settings.h"
#include "scan.h"
#include "multiview.h"
#include "announce.h"

#include <cstdio>
#include <algorithm>

using gfx::theme;
using ui::Align;
using ui::ButtonStyle;
using ui::Font;

namespace {
constexpr float kRowHeight = 58.0f;
constexpr float kHeaderH   = 60.0f;
constexpr float kFooterH   = 132.0f;

bool copy_to_clipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) { CloseClipboard(); return false; }

    if (void* target = GlobalLock(handle)) {
        memcpy(target, text.c_str(), bytes);
        GlobalUnlock(handle);
        SetClipboardData(CF_UNICODETEXT, handle);
    } else {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
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

void SourcesWindow::open_or_focus() {
    if (hwnd_) {
        if (!visible()) show();
        else {
            ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
        }
        invalidate();
        return;
    }

    if (!create(L"OMT Mini", 560, 620, true)) return;

    Settings& cfg = settings();
    if (cfg.sources_x >= 0 && cfg.sources_y >= 0) {
        SetWindowPos(hwnd_, nullptr, cfg.sources_x, cfg.sources_y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        center_on_cursor();
    }
    show();
}

bool SourcesWindow::on_close_request() {
    // Closing returns to the tray rather than exiting, which is what a tray
    // application is expected to do.
    if (hwnd_) {
        RECT rc{};
        if (GetWindowRect(hwnd_, &rc)) {
            Settings& cfg = settings();
            cfg.sources_x = rc.left;
            cfg.sources_y = rc.top;
            cfg.save();
        }
    }
    hide();
    return false;
}

void SourcesWindow::draw_header(ui::Ctx& ctx) {
    ctx.fill_rect(D2D1::RectF(0, 0, ctx.width(), kHeaderH), theme().panel);
    ctx.separator(0, ctx.width(), kHeaderH - 1.0f);

    ctx.text(ui::rect(20.0f, 0, 260.0f, kHeaderH), L"Sources", Font::Title, theme().text);

    const size_t count = cached_.size();
    ctx.text(ui::rect(20.0f, kHeaderH * 0.5f + 6.0f, 300.0f, 18.0f),
             count == 0 ? L"Looking for OMT sources on the network"
                        : fmt(L"%zu available", count),
             Font::Small, theme().text_dim, Align::Left, false);

    if (ctx.button(11, ui::rect(ctx.width() - 112.0f, 16.0f, 92.0f, 28.0f), L"Settings",
                   ButtonStyle::Normal))
        App::instance().show_settings(0);

    // Add a source without going through Settings.
    if (ctx.icon_button(13, ui::rect(ctx.width() - 148.0f, 16.0f, 28.0f, 28.0f),
                        adding_ ? ui::Ctx::Glyph::Close : ui::Ctx::Glyph::Plus)) {
        adding_ = !adding_;
        editing_address_.clear();
        add_address_text_.clear();
        add_name_text_.clear();
        add_error_.clear();
        invalidate();
    }

    // A quiet indicator rather than a dialog. Pressing it opens the About tab,
    // where the install button lives.
    if (updater().update_available()) {
        const UpdateInfo up = updater().info();
        const std::wstring label =
            up.state == UpdateState::ReadyToInstall
                ? std::wstring(L"Install update")
                : fmt(L"Update to %S", up.latest_version.c_str());
        const float w = ctx.text_width(label, Font::Small) + 26.0f;
        if (ctx.button(12, ui::rect(ctx.width() - 124.0f - w, 16.0f, w, 28.0f), label,
                       ButtonStyle::Primary))
            App::instance().show_settings(7);
    }
}

bool SourcesWindow::on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (msg == WM_OMT_SCAN) {
        if (wp == 1) scan_pending_ = true;
        invalidate();
        result = 0;
        return true;
    }
    return false;
}

// Takes the results of a finished walk and turns them into saved sources.
void SourcesWindow::finish_scan() {
    scan_pending_ = false;
    const ScanState scan = port_scanner().state();

    if (scan.hits.empty()) {
        add_error_ = L"Nothing on " + util::widen(scan.host) +
                     L" answered as an OMT sender. Press Add again to save it anyway.";
        // A second press with the address unchanged saves it without scanning.
        add_address_text_ = util::widen(omt::normalize_address(scan.host));
        return;
    }

    Settings& cfg = settings();
    const std::wstring label = util::trim(util::narrow(add_name_text_)).empty()
                             ? std::wstring()
                             : add_name_text_;
    int added = 0;
    for (const auto& hit : scan.hits) {
        const bool exists = std::any_of(
            cfg.manual_sources.begin(), cfg.manual_sources.end(),
            [&](const ManualSource& m) { return m.address == hit.address; });
        if (exists) continue;

        ManualSource entry;
        entry.address = hit.address;
        // Named after what it says it is, which beats a bare port number, and
        // never left blank: a nameless source is one other applications on the
        // network will not list.
        const std::string preferred = !hit.product.empty()
            ? hit.product
            : (label.empty() ? std::string()
                             : util::narrow(label) + " " + std::to_string(hit.port));
        std::vector<std::string> taken;
        for (const auto& other : cfg.manual_sources) taken.push_back(other.name);
        entry.name = omt::safe_source_name(preferred, omt::default_direct_name(taken));
        cfg.manual_sources.push_back(std::move(entry));
        ++added;
    }

    if (added > 0) {
        cfg.save();
        discovery().set_manual_sources(cfg.manual_sources);
    }

    toast_ = added == 1 ? std::wstring(L"Added 1 source")
                        : fmt(L"Added %d sources", added);
    if (added == 0) toast_ = L"Those were already in the list";
    toast_until_ms_ = util::now_ms() + 3000;

    add_address_text_.clear();
    add_name_text_.clear();
    add_error_.clear();
    adding_ = false;
}

void SourcesWindow::commit_add() {
    Settings& cfg = settings();
    const std::string typed = util::trim(util::narrow(add_address_text_));

    // Changing an existing entry is taken literally: someone editing an address
    // means that address, not a fresh walk of the machine.
    if (!editing_address_.empty()) {
        const std::string normalized = omt::normalize_address(typed);
        if (normalized.empty()) {
            add_error_ = L"That address could not be understood.";
            return;
        }
        const bool clashes = std::any_of(
            cfg.manual_sources.begin(), cfg.manual_sources.end(),
            [&](const ManualSource& m) {
                return m.address == normalized && m.address != editing_address_;
            });
        if (clashes) {
            add_error_ = L"Another entry already uses that address.";
            return;
        }

        for (auto& entry : cfg.manual_sources) {
            if (entry.address != editing_address_) continue;
            entry.address = normalized;
            {
                std::vector<std::string> taken;
                for (const auto& other : cfg.manual_sources)
                    if (other.address != editing_address_) taken.push_back(other.name);
                entry.name = omt::safe_source_name(util::narrow(add_name_text_),
                                                   omt::default_direct_name(taken));
            }
            break;
        }
        cfg.save();
        discovery().set_manual_sources(cfg.manual_sources);

        if (selected_address_ == editing_address_) selected_address_ = normalized;
        editing_address_.clear();
        add_address_text_.clear();
        add_name_text_.clear();
        add_error_.clear();
        adding_ = false;
        toast_ = L"Source updated";
        toast_until_ms_ = util::now_ms() + 2500;
        return;
    }

    // No port given means the machine probably has more than one sender on it,
    // so walk the range rather than assuming only the first.
    if (!typed.empty() && !omt::has_explicit_port(typed) && add_error_.empty()) {
        std::string host, port;
        const std::string probe = omt::normalize_address(typed, cfg.port_start);
        if (omt::split_address(probe, &host, &port)) {
            port_scanner().start(host, cfg.port_start, cfg.port_end, 10, hwnd());
            invalidate();
            return;
        }
    }

    const std::string normalized = omt::normalize_address(util::narrow(add_address_text_));
    if (normalized.empty()) {
        add_error_ = L"That address could not be understood.";
        return;
    }
    const bool exists = std::any_of(cfg.manual_sources.begin(), cfg.manual_sources.end(),
                                    [&](const ManualSource& m) { return m.address == normalized; });
    if (exists) {
        add_error_ = L"That address is already in the list.";
        return;
    }

    std::vector<std::string> taken;
    for (const auto& existing : cfg.manual_sources) taken.push_back(existing.name);

    ManualSource entry;
    entry.address = normalized;
    entry.name    = omt::safe_source_name(util::narrow(add_name_text_),
                                          omt::default_direct_name(taken));
    cfg.manual_sources.push_back(std::move(entry));
    cfg.save();
    discovery().set_manual_sources(cfg.manual_sources);

    add_address_text_.clear();
    add_name_text_.clear();
    add_error_.clear();
    adding_ = false;
}

float SourcesWindow::draw_add_panel(ui::Ctx& ctx, float top) {
    if (!adding_) return top;

    if (scan_pending_) finish_scan();
    if (!adding_) return top;

    // ---- while a walk is in progress the panel becomes a progress report ----
    const ScanState scan = port_scanner().state();
    if (scan.running) {
        const D2D1_RECT_F panel = D2D1::RectF(0, top, ctx.width(), top + 62.0f);
        ctx.fill_rect(panel, theme().panel_hi);
        ctx.separator(0, ctx.width(), panel.bottom - 1.0f);

        ctx.text(ui::rect(16.0f, top + 10.0f, ctx.width() - 120.0f, 20.0f),
                 fmt(L"Looking for senders on %S", scan.host.c_str()),
                 Font::BodyBold, theme().text, Align::Left, false);
        ctx.text(ui::rect(16.0f, top + 30.0f, ctx.width() - 120.0f, 18.0f),
                 fmt(L"port %d,  %zu found", scan.current_port, scan.hits.size()),
                 Font::Small, theme().text_dim, Align::Left, false);

        if (ctx.button(34, ui::rect(ctx.width() - 96.0f, top + 16.0f, 80.0f, 28.0f),
                       L"Stop", ButtonStyle::Normal)) {
            port_scanner().cancel();
        }
        ctx.request_redraw();
        return panel.bottom;
    }

    const float height = add_error_.empty() ? 62.0f : 88.0f;
    const D2D1_RECT_F panel = D2D1::RectF(0, top, ctx.width(), top + height);
    ctx.fill_rect(panel, theme().panel_hi);
    ctx.separator(0, ctx.width(), panel.bottom - 1.0f);

    const float y = top + 10.0f;
    if (ctx.text_field(30, D2D1::RectF(16.0f, y, ctx.width() * 0.52f, y + 28.0f),
                       &add_address_text_, L"10.0.0.5  or  host:6400"))
        add_error_.clear();
    ctx.text_field(31, D2D1::RectF(ctx.width() * 0.52f + 8.0f, y,
                                   ctx.width() - 176.0f, y + 28.0f),
                   &add_name_text_, L"name (optional)");

    const bool editing = !editing_address_.empty();
    const bool can_add = !util::trim(util::narrow(add_address_text_)).empty();
    if (ctx.button(32, ui::rect(ctx.width() - 168.0f, y, 72.0f, 28.0f),
                   editing ? L"Save" : L"Add", ButtonStyle::Primary, can_add) ||
        (can_add && ctx.input().key == VK_RETURN)) {
        commit_add();
        invalidate();
    }
    if (ctx.button(33, ui::rect(ctx.width() - 88.0f, y, 72.0f, 28.0f), L"Cancel",
                   ButtonStyle::Normal) ||
        ctx.input().key == VK_ESCAPE) {
        adding_ = false;
        editing_address_.clear();
        add_error_.clear();
        invalidate();
    }

    if (add_error_.empty()) {
        ctx.text(ui::rect(16.0f, y + 30.0f, ctx.width() - 32.0f, 18.0f),
                 editing
                     ? L"Editing an existing source. The address is used exactly as typed."
                     : L"Leave the port off and OMT Mini finds every sender on that machine.",
                 Font::Small, theme().text_dim, Align::Left, false);
    } else {
        ctx.text_wrapped(D2D1::RectF(16.0f, y + 30.0f, ctx.width() - 32.0f, y + 74.0f),
                         add_error_, Font::Small, theme().warn);
    }

    return panel.bottom;
}

void SourcesWindow::draw_list(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    if (cached_.empty()) {
        ctx.text(D2D1::RectF(area.left, area.top + 60.0f, area.right, area.top + 90.0f),
                 L"No sources yet", Font::Body, theme().text_dim, Align::Center);
        ctx.text(D2D1::RectF(area.left + 40.0f, area.top + 88.0f,
                             area.right - 40.0f, area.top + 150.0f),
                 L"Start an OMT sender on this machine or another one on the same "
                 L"network. Sources appear here automatically. If one cannot be "
                 L"discovered, add it by address under Settings > Sources.",
                 Font::Small, theme().text_dim, Align::Center);
        return;
    }

    scroll_.content_height = cached_.size() * kRowHeight + 8.0f;
    std::string remove_address;
    std::string edit_address;
    ctx.begin_scroll(1, area, &scroll_);

    float y = area.top + 4.0f;
    for (size_t i = 0; i < cached_.size(); ++i) {
        const auto& s = cached_[i];
        const D2D1_RECT_F rowr =
            D2D1::RectF(area.left + 12.0f, y, area.right - 12.0f, y + kRowHeight - 6.0f);

        // Cull rows scrolled out of view.
        if (rowr.bottom - scroll_.offset < area.top ||
            rowr.top - scroll_.offset > area.bottom) {
            y += kRowHeight;
            continue;
        }

        // Stride of eight: a row now claims id+0 through id+4, and a stride of
        // four would put every row's last widget on the next row's first.
        const auto id = static_cast<ui::Id>(100 + i * 8);
        const bool over = ctx.hovered(rowr);
        const bool selected = !selected_address_.empty() && selected_address_ == s.address;
        const float t = ctx.animate(id, (over || selected) ? 1.0f : 0.0f);

        // Selecting a row is what reveals Remove, so the whole row is a click
        // target except where the action buttons sit.
        const D2D1_RECT_F actions =
            D2D1::RectF(rowr.right - 348.0f, rowr.top, rowr.right, rowr.bottom);
        if (!ctx.hovered(actions) && ctx.clicked_area(id, rowr)) {
            selected_address_ = selected ? std::string() : s.address;
            invalidate();
        }

        ctx.fill_rect(rowr, (over || selected) ? theme().panel_hi : theme().panel,
                      ui::metric::kRadius);
        if (t > 0.01f) {
            auto edge = selected ? theme().accent : theme().accent;
            edge.a = selected ? 0.9f : t * 0.5f;
            ctx.stroke_rect(rowr, edge, selected ? 1.6f : 1.0f, ui::metric::kRadius);
        }

        // Status dot. Discovered sources are online by definition, so this only
        // ever says anything interesting for one added by hand.
        D2D1_COLOR_F dot = theme().ok;
        if (s.is_manual) {
            dot = s.status == SourceStatus::Online  ? theme().ok
                : s.status == SourceStatus::Offline ? theme().danger
                                                    : theme().text_dim;
        }
        ctx.status_dot(rowr.left + 18.0f, rowr.top + 16.0f, 3.5f, dot);

        const float text_left = rowr.left + 32.0f;
        const std::wstring name = util::widen(s.name);
        ctx.text(ui::rect(text_left, rowr.top + 8.0f, 260.0f, 20.0f),
                 name, Font::BodyBold, theme().text, Align::Left, false);

        // Tags sit after the name. The hover actions are given room whether or
        // not they are showing, so nothing jumps when the pointer arrives.
        float tag_x = text_left + std::min(230.0f, ctx.text_width(name, Font::BodyBold))
                    + 10.0f;
        const float tag_limit = rowr.right - 352.0f;
        const float tag_y = rowr.top + 9.0f;

        auto add_tag = [&](const std::wstring& label, const D2D1_COLOR_F& colour) {
            if (label.empty()) return;
            const float width = ctx.text_width(label, Font::Small) + 14.0f;
            if (tag_x + width > tag_limit) return;
            tag_x += ctx.badge(tag_x, tag_y, 17.0f, label, colour) + 6.0f;
        };

        if (s.is_manual)     add_tag(L"direct", theme().badge_direct);
        else if (s.is_local) add_tag(L"this machine", theme().accent_hi);

        // Being announced means the rest of the network can see it too, which
        // is worth saying on the row rather than only in settings.
        if (s.is_manual && announcer().is_announced(s.address))
            add_tag(L"mDNS", theme().ok);

        // What the sender says it is. Green for another OMT Mini, so a feed
        // that will behave exactly like this one is obvious at a glance.
        if (s.is_omt_mini) {
            add_tag(L"OMT Mini", theme().ok);
        } else if (!s.manufacturer.empty() || !s.product.empty()) {
            std::wstring maker =
                util::widen(!s.manufacturer.empty() ? s.manufacturer : s.product);
            if (maker.size() > 20) maker = maker.substr(0, 19) + L"\u2026";
            add_tag(maker, theme().text_dim);
        }

        // Second line is where you look for something to type into another
        // machine, so it always carries an address.
        std::wstring sub;
        if (s.is_manual) {
            sub = util::widen(s.address);
        } else {
            sub = util::widen(s.host);
            if (!s.ip.empty()) sub += L"   " + util::widen(s.ip);
        }
        if (s.is_manual && s.status == SourceStatus::Offline) sub += L"   not answering";
        ctx.text(ui::rect(text_left, rowr.top + 28.0f, 360.0f, 18.0f),
                 sub, Font::Small, theme().text_dim, Align::Left, false);

        // Actions appear on hover, or while the row is selected, to keep the
        // list calm at rest.
        if (over || selected || t > 0.3f) {
            const float bw = 76.0f;
            // Still openable when offline: a viewer shows "connecting" and
            // picks the source up the moment it comes back.
            if (ctx.button(id + 1,
                           ui::rect(rowr.right - bw - 12.0f, rowr.top + 13.0f, bw, 26.0f),
                           L"View", ButtonStyle::Primary)) {
                App::instance().open_viewer(s.address);
            }
            if (ctx.button(id + 2,
                           ui::rect(rowr.right - bw - 12.0f - 96.0f, rowr.top + 13.0f,
                                    88.0f, 26.0f),
                           L"To webcam", ButtonStyle::Normal)) {
                Settings& cfg = settings();
                cfg.webcam_source = s.address;
                cfg.save();
                webcam().stop();
                App::instance().toggle_webcam();
                invalidate();
            }
            // Only a source that was added by hand can be edited or removed. A
            // discovered one is not ours to change: it would simply come back.
            if (s.is_manual && selected) {
                if (ctx.button(id + 3,
                               ui::rect(rowr.right - bw - 12.0f - 96.0f - 68.0f,
                                        rowr.top + 13.0f, 60.0f, 26.0f),
                               L"Edit", ButtonStyle::Normal)) {
                    edit_address = s.address;
                }
                if (ctx.button(id + 4,
                               ui::rect(rowr.right - bw - 12.0f - 96.0f - 68.0f - 84.0f,
                                        rowr.top + 13.0f, 76.0f, 26.0f),
                               L"Remove", ButtonStyle::Danger)) {
                    remove_address = s.address;
                }
            }
        }
        y += kRowHeight;
    }

    ctx.end_scroll();

    if (!edit_address.empty()) {
        const Settings& cfg = settings();
        for (const auto& entry : cfg.manual_sources) {
            if (entry.address != edit_address) continue;
            editing_address_  = entry.address;
            add_address_text_ = util::widen(entry.address);
            add_name_text_    = util::widen(entry.name);
            add_error_.clear();
            adding_ = true;
            break;
        }
        invalidate();
    }

    if (!remove_address.empty()) {
        Settings& cfg = settings();
        cfg.manual_sources.erase(
            std::remove_if(cfg.manual_sources.begin(), cfg.manual_sources.end(),
                           [&](const ManualSource& m) { return m.address == remove_address; }),
            cfg.manual_sources.end());
        cfg.save();
        discovery().set_manual_sources(cfg.manual_sources);
        if (selected_address_ == remove_address) selected_address_.clear();
        invalidate();
    }
}

void SourcesWindow::draw_footer(ui::Ctx& ctx, const D2D1_RECT_F& area) {
    ctx.fill_rect(area, theme().panel);
    ctx.separator(0, ctx.width(), area.top);

    const auto cap = desktop_capture().stats();
    const auto cam = webcam().stats();

    // ---- desktop capture ----
    const float y0 = area.top + 14.0f;
    ctx.text(ui::rect(20.0f, y0, 200.0f, 18.0f), L"Desktop capture", Font::BodyBold,
             theme().text, Align::Left, false);

    std::wstring cap_sub;
    if (cap.running && cap.width > 0 && !cap.connect_url.empty()) {
        // The address someone on another subnet would have to type in, which is
        // otherwise impossible to find: libomt does not report the bound port.
        cap_sub = fmt(L"%dx%d  %.0f fps  %d receiver%s   %S", cap.width, cap.height,
                      cap.fps, cap.connections, cap.connections == 1 ? L"" : L"s",
                      cap.connect_url.c_str());
    } else if (cap.running && cap.width > 0) {
        cap_sub = fmt(L"%dx%d  %.0f fps  %d receiver%s", cap.width, cap.height,
                      cap.fps, cap.connections, cap.connections == 1 ? L"" : L"s");
    } else if (!cap.error.empty()) {
        cap_sub = util::widen(cap.error);
    } else {
        cap_sub = L"Share this screen as an OMT source";
    }
    ctx.text(ui::rect(20.0f, y0 + 18.0f, ctx.width() - 160.0f, 18.0f), cap_sub,
             Font::Small, cap.error.empty() ? theme().text_dim : theme().warn,
             Align::Left, false);

    if (ctx.button(21, ui::rect(ctx.width() - 112.0f, y0 - 2.0f, 92.0f, 28.0f),
                   desktop_capture().running() ? L"Stop" : L"Start",
                   desktop_capture().running() ? ButtonStyle::Danger : ButtonStyle::Normal)) {
        App::instance().toggle_desktop_capture();
        invalidate();
    }

    if (!cap.connect_url.empty()) {
        if (ctx.button(23, ui::rect(ctx.width() - 178.0f, y0 - 2.0f, 60.0f, 28.0f),
                       L"Copy", ButtonStyle::Ghost)) {
            if (copy_to_clipboard(hwnd(), util::widen(cap.connect_url))) {
                toast_ = L"Address copied";
                toast_until_ms_ = util::now_ms() + 2500;
            }
        }
    }

    // ---- webcam ----
    const float y1 = area.top + 54.0f;
    ctx.text(ui::rect(20.0f, y1, 200.0f, 18.0f), L"Webcam output", Font::BodyBold,
             theme().text, Align::Left, false);

    std::wstring cam_sub;
    if (cam.running && cam.connected) {
        cam_sub = fmt(L"%S  ->  %dx%d  %.0f fps",
                      omt::short_name(cam.source).c_str(), cam.width, cam.height, cam.fps);
    } else if (cam.running) {
        cam_sub = L"Waiting for the source";
    } else if (settings().webcam_source.empty()) {
        cam_sub = L"Pick a source to appear as a camera in other apps";
    } else {
        cam_sub = util::widen(omt::short_name(settings().webcam_source));
    }
    ctx.text(ui::rect(20.0f, y1 + 18.0f, ctx.width() - 160.0f, 18.0f), cam_sub,
             Font::Small, theme().text_dim, Align::Left, false);

    if (ctx.button(22, ui::rect(ctx.width() - 112.0f, y1 - 2.0f, 92.0f, 28.0f),
                   webcam().running() ? L"Stop" : L"Start",
                   webcam().running() ? ButtonStyle::Danger : ButtonStyle::Normal)) {
        App::instance().toggle_webcam();
        invalidate();
    }

    // ---- multiview output ----
    const auto mv = multiview_output().stats();
    const float y2 = area.top + 94.0f;
    ctx.text(ui::rect(20.0f, y2, 200.0f, 18.0f), L"Multiview output", Font::BodyBold,
             theme().text, Align::Left, false);

    std::wstring mv_sub;
    if (mv.running && mv.fps > 0.0f) {
        mv_sub = fmt(L"%dx%d  %.0f fps  %d receiver%s", mv.width, mv.height, mv.fps,
                     mv.connections, mv.connections == 1 ? L"" : L"s");
        if (!mv.connect_url.empty()) mv_sub += L"   " + util::widen(mv.connect_url);
    } else if (mv.running) {
        mv_sub = L"Starting";
    } else if (!mv.error.empty()) {
        mv_sub = util::widen(mv.error);
    } else {
        mv_sub = L"Send the multiview to other machines as one source";
    }
    ctx.text(ui::rect(20.0f, y2 + 18.0f, ctx.width() - 240.0f, 18.0f), mv_sub,
             Font::Small, mv.error.empty() ? theme().text_dim : theme().warn,
             Align::Left, false);

    if (ctx.button(24, ui::rect(ctx.width() - 112.0f, y2 - 2.0f, 92.0f, 28.0f),
                   multiview_output().running() ? L"Stop" : L"Start",
                   multiview_output().running() ? ButtonStyle::Danger : ButtonStyle::Normal)) {
        App::instance().toggle_multiview_output();
        invalidate();
    }

    // Open the wall itself, which the output does not require.
    if (ctx.button(25, ui::rect(ctx.width() - 210.0f, y2 - 2.0f, 90.0f, 28.0f),
                   L"Open wall", ButtonStyle::Normal))
        App::instance().show_multiview();
}

void SourcesWindow::on_render(ui::Ctx& ctx) {
    cached_ = discovery().sources();

    ctx.fill_rect(D2D1::RectF(0, 0, ctx.width(), ctx.height()), theme().bg);

    draw_header(ctx);
    const float list_top = draw_add_panel(ctx, kHeaderH);

    const D2D1_RECT_F footer =
        D2D1::RectF(0, ctx.height() - kFooterH, ctx.width(), ctx.height());
    const D2D1_RECT_F list =
        D2D1::RectF(0, list_top, ctx.width(), footer.top);

    draw_list(ctx, list);
    draw_footer(ctx, footer);

    if (!toast_.empty() && util::now_ms() < toast_until_ms_) {
        const float w = ctx.text_width(toast_, Font::Small) + 28.0f;
        const D2D1_RECT_F pill = D2D1::RectF((ctx.width() - w) * 0.5f, ctx.height() - 132.0f,
                                             (ctx.width() + w) * 0.5f, ctx.height() - 104.0f);
        ctx.fill_rect(pill, gfx::rgb(0x0B0D10, 0.92f), 14.0f);
        ctx.stroke_rect(pill, theme().border, 1.0f, 14.0f);
        ctx.text(pill, toast_, Font::Small, theme().text, Align::Center);
        ctx.request_redraw();
    }

    // Keep the footer counters live while something is running.
    if (desktop_capture().running() || webcam().running() ||
        multiview_output().running())
        ctx.request_redraw();
}
