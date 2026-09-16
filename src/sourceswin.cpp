#include "sourceswin.h"
#include "app.h"
#include "capture.h"
#include "webcam.h"
#include "settings.h"
#include "omt.h"
#include "update.h"

#include <cstdio>
#include <algorithm>

using gfx::theme;
using ui::Align;
using ui::ButtonStyle;
using ui::Font;

namespace {
constexpr float kRowHeight = 58.0f;
constexpr float kHeaderH   = 60.0f;
constexpr float kFooterH   = 92.0f;

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
            App::instance().show_settings(6);
    }
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

        const auto id = static_cast<ui::Id>(100 + i * 4);
        const bool over = ctx.hovered(rowr);
        const float t = ctx.animate(id, over ? 1.0f : 0.0f);

        ctx.fill_rect(rowr, over ? theme().panel_hi : theme().panel, ui::metric::kRadius);
        if (t > 0.01f) {
            auto edge = theme().accent;
            edge.a = t * 0.5f;
            ctx.stroke_rect(rowr, edge, 1.0f, ui::metric::kRadius);
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
        const float tag_limit = rowr.right - 210.0f;
        const float tag_y = rowr.top + 9.0f;

        auto add_tag = [&](const std::wstring& label, const D2D1_COLOR_F& colour) {
            if (label.empty()) return;
            const float width = ctx.text_width(label, Font::Small) + 14.0f;
            if (tag_x + width > tag_limit) return;
            tag_x += ctx.badge(tag_x, tag_y, 17.0f, label, colour) + 6.0f;
        };

        if (s.is_manual)     add_tag(L"direct", theme().badge_direct);
        else if (s.is_local) add_tag(L"this machine", theme().accent_hi);

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

        std::wstring sub = s.is_manual ? util::widen(s.address) : util::widen(s.host);
        if (s.is_manual && s.status == SourceStatus::Offline) sub += L"   not answering";
        ctx.text(ui::rect(text_left, rowr.top + 28.0f, 320.0f, 18.0f),
                 sub, Font::Small, theme().text_dim, Align::Left, false);

        // Actions appear on hover to keep the list calm at rest.
        if (over || t > 0.3f) {
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
        }
        y += kRowHeight;
    }

    ctx.end_scroll();
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
    if (cap.running && cap.width > 0) {
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
}

void SourcesWindow::on_render(ui::Ctx& ctx) {
    cached_ = discovery().sources();

    ctx.fill_rect(D2D1::RectF(0, 0, ctx.width(), ctx.height()), theme().bg);

    draw_header(ctx);

    const D2D1_RECT_F footer =
        D2D1::RectF(0, ctx.height() - kFooterH, ctx.width(), ctx.height());
    const D2D1_RECT_F list =
        D2D1::RectF(0, kHeaderH, ctx.width(), footer.top);

    draw_list(ctx, list);
    draw_footer(ctx, footer);

    // Keep the footer counters live while something is running.
    if (desktop_capture().running() || webcam().running()) ctx.request_redraw();
}
