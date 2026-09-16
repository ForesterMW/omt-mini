#include "viewer.h"
#include "settings.h"

#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using gfx::theme;
using ui::Align;
using ui::ButtonStyle;
using ui::Font;

namespace {
constexpr int64_t kChromeTimeoutMs = 2600;
constexpr float   kBarHeight = 46.0f;
constexpr UINT_PTR kChromeTimer = 1;

const std::vector<std::wstring>& quality_items() {
    static const std::vector<std::wstring> items = { L"Auto", L"Low", L"Medium", L"High" };
    return items;
}

OMTQuality quality_from_index(int i) {
    switch (i) {
        case 1:  return OMTQuality_Low;
        case 2:  return OMTQuality_Medium;
        case 3:  return OMTQuality_High;
        default: return OMTQuality_Default;
    }
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

ViewerWindow::ViewerWindow(const std::string& address) : address_(address) {
    title_ = util::widen(omt::short_name(address));
    if (title_.empty()) title_ = util::widen(address);

    const Settings& cfg = settings();
    show_stats_    = cfg.viewer_show_stats;
    on_top_        = cfg.viewer_always_on_top;
    low_bandwidth_ = cfg.viewer_low_bandwidth;
    volume_        = std::clamp(cfg.viewer_volume / 100.0f, 0.0f, 1.0f);
    muted_         = !cfg.viewer_audio;

    switch (cfg.viewer_quality) {
        case OMTQuality_Low:    quality_index_ = 1; break;
        case OMTQuality_Medium: quality_index_ = 2; break;
        case OMTQuality_High:   quality_index_ = 3; break;
        default:                quality_index_ = 0; break;
    }
}

ViewerWindow::~ViewerWindow() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    audio_.stop();
    receiver_.close();
}

bool ViewerWindow::open() {
    const Settings& cfg = settings();
    const std::wstring caption = L"OMT Mini  -  " + title_;
    if (!create(caption, cfg.viewer_width, cfg.viewer_height, true)) return false;

    set_always_on_top(on_top_);
    audio_.set_volume(volume_);
    audio_.set_muted(muted_);
    last_activity_ms_ = util::now_ms();

    running_ = true;
    thread_  = std::thread([this] { receive_loop(); });

    show();
    return true;
}

void ViewerWindow::on_closing() {
    // Withdraw this viewer's tally before disconnecting, so a source is not
    // left lit by a window that has gone.
    if ((my_pgm_ || my_pvw_) && receiver_.is_open())
        receiver_.set_tally(false, false);

    running_ = false;
    closed_  = true;
    if (thread_.joinable()) thread_.join();
    audio_.stop();
    receiver_.close();

    // Remember the last used window size for the next viewer.
    if (hwnd_ && !fullscreen_) {
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            Settings& cfg = settings();
            cfg.viewer_width  = MulDiv(rc.right - rc.left, 96, static_cast<int>(dpi_));
            cfg.viewer_height = MulDiv(rc.bottom - rc.top, 96, static_cast<int>(dpi_));
            cfg.save();
        }
    }
    destroy();
}

void ViewerWindow::apply_flags() {
    OMTReceiveFlags flags = OMTReceiveFlags_None;
    if (low_bandwidth_) flags = OMTReceiveFlags_Preview;
    receiver_.set_flags(flags);
}

void ViewerWindow::receive_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Copied, not referenced: the settings window can mutate the live object
    // from the UI thread while this runs.
    const Settings cfg = settings();
    const auto format = static_cast<OMTPreferredVideoFormat>(cfg.viewer_format);

    OMTReceiveFlags flags = low_bandwidth_ ? OMTReceiveFlags_Preview : OMTReceiveFlags_None;
    const OMTFrameType types = static_cast<OMTFrameType>(
        OMTFrameType_Video | OMTFrameType_Audio | OMTFrameType_Metadata);

    if (!receiver_.open(address_, types, format, flags)) {
        util::logf("viewer: could not create receiver for '%s'", address_.c_str());
        CoUninitialize();
        return;
    }
    receiver_.set_suggested_quality(quality_from_index(quality_index_));

    int64_t window_start = util::now_ms();
    int     window_frames = 0;
    int64_t last_stats_ms = 0;

    while (running_) {
        OMTMediaFrame* f = receiver_.receive(types, 100);
        if (!f) {
            if (util::now_ms() - last_frame_ms_.load() > 1500) connected_ = false;
            if (hwnd_) PostMessageW(hwnd_, WM_OMT_FRAME, 0, 0);
            continue;
        }

        if (f->Type == OMTFrameType_Video) {
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                if (texture_.upload(*f)) {
                    unsupported_format_ = false;
                } else {
                    unsupported_format_ = true;
                }
            }
            frame_w_     = f->Width;
            frame_h_     = f->Height;
            aspect_      = f->AspectRatio;
            codec_       = static_cast<int>(f->Codec);
            interlaced_  = (f->Flags & OMTVideoFlags_Interlaced) != 0;
            connected_   = true;
            last_frame_ms_ = util::now_ms();
            ++window_frames;

            if (f->FrameMetadata && f->FrameMetadataLength > 1) {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                metadata_.push_back(util::widen(static_cast<const char*>(f->FrameMetadata)));
                while (metadata_.size() > 40) metadata_.pop_front();
            }

            if (hwnd_) PostMessageW(hwnd_, WM_OMT_FRAME, 0, 0);

        } else if (f->Type == OMTFrameType_Audio) {
            audio_channels_ = f->Channels;
            audio_rate_     = f->SampleRate;
            audio_.push(*f);

        } else if (f->Type == OMTFrameType_Metadata) {
            if (f->Data && f->DataLength > 1) {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                metadata_.push_back(util::widen(static_cast<const char*>(f->Data)));
                while (metadata_.size() > 40) metadata_.pop_front();
            }
        }

        // Stats once a second.
        const int64_t now = util::now_ms();
        if (now - window_start >= 1000) {
            OMTStatistics vs{};
            receiver_.video_stats(&vs);
            const int64_t elapsed = now - window_start;
            fps_        = static_cast<float>(window_frames) * 1000.0f / elapsed;
            mbps_       = static_cast<float>(vs.BytesReceivedSinceLast) * 8.0f /
                          static_cast<float>(elapsed) / 1000.0f;
            frames_     = vs.Frames;
            dropped_    = vs.FramesDropped;
            codec_time_ = vs.CodecTimeSinceLast;
            window_frames = 0;
            window_start = now;

            {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                if (sender_product_.empty()) {
                    OMTSenderInfo info{};
                    if (receiver_.sender_info(&info))
                        sender_product_ = util::widen(info.ProductName);
                }
            }

            // Tally is polled here rather than from the UI thread so every
            // libomt call for this receiver stays on one thread.
            OMTTally tally{};
            if (receiver_.get_tally(0, &tally)) {
                agg_pgm_ = tally.program != 0;
                agg_pvw_ = tally.preview != 0;
            }
        }
        (void)last_stats_ms;
    }

    receiver_.close();
    CoUninitialize();
}

bool ViewerWindow::chrome_visible() const {
    return (util::now_ms() - last_activity_ms_) < kChromeTimeoutMs;
}

void ViewerWindow::toggle_fullscreen() {
    if (!hwnd_) return;

    if (!fullscreen_) {
        saved_placement_.length = sizeof(saved_placement_);
        GetWindowPlacement(hwnd_, &saved_placement_);
        saved_style_ = GetWindowLongW(hwnd_, GWL_STYLE);

        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(mon, &mi)) return;

        SetWindowLongW(hwnd_, GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, saved_style_);
        SetWindowPlacement(hwnd_, &saved_placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
        fullscreen_ = false;
        set_always_on_top(on_top_);
    }
    last_activity_ms_ = util::now_ms();
    invalidate();
}

bool ViewerWindow::on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    switch (msg) {
        case WM_MOUSEMOVE:
            last_activity_ms_ = util::now_ms();
            if (!timer_running_) {
                SetTimer(hwnd_, kChromeTimer, 250, nullptr);
                timer_running_ = true;
            }
            break;

        case WM_TIMER:
            if (wp == kChromeTimer) {
                invalidate();
                if (!chrome_visible()) {
                    KillTimer(hwnd_, kChromeTimer);
                    timer_running_ = false;
                }
                result = 0;
                return true;
            }
            break;

        case WM_SETCURSOR:
            // Hide the pointer with the chrome while full screen.
            if (fullscreen_ && LOWORD(lp) == HTCLIENT && !chrome_visible()) {
                SetCursor(nullptr);
                result = TRUE;
                return true;
            }
            break;

        case WM_KEYDOWN:
            last_activity_ms_ = util::now_ms();
            switch (wp) {
                case VK_F11:
                    toggle_fullscreen();
                    result = 0;
                    return true;
                case VK_ESCAPE:
                    if (fullscreen_) { toggle_fullscreen(); result = 0; return true; }
                    break;
                case 'F':
                    toggle_fullscreen();
                    result = 0;
                    return true;
                case 'M':
                    muted_ = !muted_;
                    audio_.set_muted(muted_);
                    invalidate();
                    result = 0;
                    return true;
                case 'S':
                    show_stats_ = !show_stats_;
                    invalidate();
                    result = 0;
                    return true;
                case 'D':
                    show_metadata_ = !show_metadata_;
                    invalidate();
                    result = 0;
                    return true;
                case 'T':
                    on_top_ = !on_top_;
                    set_always_on_top(on_top_);
                    invalidate();
                    result = 0;
                    return true;
                default:
                    break;
            }
            break;

        case WM_LBUTTONDBLCLK:
            toggle_fullscreen();
            result = 0;
            return true;

        default:
            break;
    }
    return false;
}

void ViewerWindow::on_render_video() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!texture_.valid()) return;

    // Runs in the Direct3D pass, so everything here is in pixels.
    const D2D1_RECT_F bounds = D2D1::RectF(0, 0,
                                           static_cast<float>(surface_.width()),
                                           static_cast<float>(surface_.height()));
    const D2D1_RECT_F fitted =
        gfx::fit_rect(bounds, texture_.width(), texture_.height(), aspect_.load());
    texture_.draw(surface_, fitted);
}

void ViewerWindow::draw_status(ui::Ctx& ctx) {
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (connected_ && texture_.valid()) return;
    }

    const std::wstring msg =
        unsupported_format_ ? L"Unsupported pixel format"
      : connected_          ? L"Waiting for video"
                            : L"Connecting to " + title_;

    const float cy = ctx.height() * 0.5f;
    ctx.text(D2D1::RectF(0, cy - 16.0f, ctx.width(), cy + 16.0f), msg,
             Font::Title, theme().text_dim, Align::Center);

    if (unsupported_format_) {
        ctx.text(D2D1::RectF(0, cy + 16.0f, ctx.width(), cy + 40.0f),
                 L"Try a different Preferred format in Settings > Viewer.",
                 Font::Small, theme().text_dim, Align::Center);
    }
}

void ViewerWindow::draw_top_bar(ui::Ctx& ctx, float alpha) {
    if (alpha <= 0.01f) return;

    const D2D1_RECT_F bar = D2D1::RectF(0, 0, ctx.width(), kBarHeight);
    ctx.fill_rect(bar, gfx::rgb(0x0B0D10, 0.72f * alpha));

    auto dim  = theme().text_dim;  dim.a  *= alpha;
    auto text = theme().text;      text.a *= alpha;

    ctx.text(D2D1::RectF(14.0f, 0, ctx.width() * 0.6f, kBarHeight), title_,
             Font::BodyBold, text);

    const int w = frame_w_, h = frame_h_;
    std::wstring info;
    if (w > 0 && h > 0) {
        info = fmt(L"%dx%d%s  %.0f fps  %S  %.1f Mb/s", w, h,
                   interlaced_ ? L"i" : L"p", fps_.load(),
                   omt::codec_name(static_cast<OMTCodec>(codec_.load())), mbps_.load());
    }
    ctx.text(D2D1::RectF(ctx.width() * 0.45f, 0, ctx.width() - 14.0f, kBarHeight),
             info, Font::Small, dim, Align::Right);

    // The strip shows the source's tally across every receiver connected to
    // it, not just this one, which is what its camera lamp will be doing.
    const bool pgm = agg_pgm_.load();
    const bool pvw = agg_pvw_.load();
    if (pgm || pvw) {
        auto c = pgm ? theme().tally_pgm : theme().tally_pvw;
        c.a *= alpha;
        ctx.fill_rect(D2D1::RectF(0, 0, ctx.width(), 3.0f), c);

        auto label = c;
        label.a = alpha;
        ctx.text(ui::rect(14.0f, kBarHeight - 4.0f, 220.0f, 16.0f),
                 pgm ? L"ON AIR at the source" : L"previewed at the source",
                 Font::Small, label, Align::Left, false);
    }
}

void ViewerWindow::draw_bottom_bar(ui::Ctx& ctx, float alpha) {
    if (alpha <= 0.01f) return;

    const float top = ctx.height() - kBarHeight;
    ctx.fill_rect(D2D1::RectF(0, top, ctx.width(), ctx.height()),
                  gfx::rgb(0x0B0D10, 0.72f * alpha));

    const float cy = top + kBarHeight * 0.5f;
    float x = 12.0f;

    // Mute
    if (ctx.icon_button(101, ui::rect(x, cy - 14.0f, 28.0f, 28.0f),
                        muted_ ? ui::Ctx::Glyph::Minus : ui::Ctx::Glyph::Dot)) {
        muted_ = !muted_;
        audio_.set_muted(muted_);
    }
    x += 34.0f;

    // Volume
    float vol = volume_;
    if (ctx.slider(102, ui::rect(x, cy - 10.0f, 96.0f, 20.0f), &vol, 0.0f, 1.0f)) {
        volume_ = vol;
        audio_.set_volume(volume_);
    }
    x += 108.0f;

    ctx.text(ui::rect(x, top, 46.0f, kBarHeight),
             fmt(L"%d%%", static_cast<int>(volume_ * 100.0f + 0.5f)),
             Font::Small, theme().text_dim);
    x += 52.0f;

    // Tally controls, sent upstream to the source.
    if (ctx.button(103, ui::rect(x, cy - 13.0f, 52.0f, 26.0f), L"PGM",
                   my_pgm_ ? ButtonStyle::Danger : ButtonStyle::Normal)) {
        my_pgm_ = !my_pgm_;
        receiver_.set_tally(my_pvw_, my_pgm_);
    }
    x += 58.0f;
    if (ctx.button(104, ui::rect(x, cy - 13.0f, 52.0f, 26.0f), L"PVW",
                   my_pvw_ ? ButtonStyle::Primary : ButtonStyle::Normal)) {
        my_pvw_ = !my_pvw_;
        receiver_.set_tally(my_pvw_, my_pgm_);
    }
    x += 58.0f;

    // Right hand cluster.
    float rx = ctx.width() - 12.0f - 28.0f;
    if (ctx.icon_button(110, ui::rect(rx, cy - 14.0f, 28.0f, 28.0f), ui::Ctx::Glyph::Pop))
        toggle_fullscreen();
    rx -= 34.0f;

    if (ctx.button(111, ui::rect(rx - 56.0f, cy - 13.0f, 84.0f, 26.0f), L"On top",
                   on_top_ ? ButtonStyle::Primary : ButtonStyle::Normal)) {
        on_top_ = !on_top_;
        set_always_on_top(on_top_);
    }
    rx -= 96.0f;

    if (ctx.button(112, ui::rect(rx - 46.0f, cy - 13.0f, 74.0f, 26.0f), L"Stats",
                   show_stats_ ? ButtonStyle::Primary : ButtonStyle::Normal))
        show_stats_ = !show_stats_;
    rx -= 86.0f;

    if (ctx.button(113, ui::rect(rx - 56.0f, cy - 13.0f, 84.0f, 26.0f), L"Low bw",
                   low_bandwidth_ ? ButtonStyle::Primary : ButtonStyle::Normal)) {
        low_bandwidth_ = !low_bandwidth_;
        apply_flags();
    }
    rx -= 96.0f;

    // Quality suggestion sent back to the sender.
    int q = quality_index_;
    if (ctx.dropdown(114, ui::rect(rx - 70.0f, cy - 13.0f, 98.0f, 26.0f),
                     quality_items(), &q)) {
        quality_index_ = q;
        receiver_.set_suggested_quality(quality_from_index(q));
    }
}

void ViewerWindow::draw_meters(ui::Ctx& ctx) {
    const int channels = std::min(audio_.meter_channels(), 8);
    if (channels <= 0) return;

    audio_.decay_meters(0.016f);

    const float w = 5.0f;
    const float gap = 3.0f;
    const float total = channels * w + (channels - 1) * gap;
    const float top = kBarHeight + 14.0f;
    const float bottom = ctx.height() - kBarHeight - 14.0f;
    const float height = bottom - top;
    if (height < 40.0f) return;

    float x = ctx.width() - 14.0f - total;
    for (int c = 0; c < channels; ++c) {
        const D2D1_RECT_F track = D2D1::RectF(x, top, x + w, bottom);
        ctx.fill_rect(track, gfx::rgb(0x000000, 0.45f), 2.0f);

        // dBFS scale, floor at -60 dB, which reads better than linear.
        const float peak = audio_.peak(c);
        float norm = 0.0f;
        if (peak > 0.0001f) {
            const float db = 20.0f * std::log10(peak);
            norm = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
        }
        const float level_top = bottom - norm * height;
        auto colour = (peak >= 0.99f) ? theme().danger
                    : (peak > 0.7f)   ? theme().warn
                                      : theme().ok;
        ctx.fill_rect(D2D1::RectF(x, level_top, x + w, bottom), colour, 2.0f);
        x += w + gap;
    }
    ctx.request_redraw();
}

void ViewerWindow::draw_stats(ui::Ctx& ctx) {
    if (!show_stats_) return;

    struct Row { const wchar_t* label; std::wstring value; };
    std::vector<Row> rows;

    rows.push_back({ L"Source", title_ });
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        if (!sender_product_.empty()) rows.push_back({ L"Sender", sender_product_ });
    }
    rows.push_back({ L"Resolution", fmt(L"%d x %d%s", frame_w_.load(), frame_h_.load(),
                                        interlaced_ ? L"i" : L"p") });
    rows.push_back({ L"Frame rate", fmt(L"%.2f fps", fps_.load()) });
    rows.push_back({ L"Codec",
                     util::widen(omt::codec_name(static_cast<OMTCodec>(codec_.load()))) });
    rows.push_back({ L"Bitrate",    fmt(L"%.2f Mb/s", mbps_.load()) });
    rows.push_back({ L"Frames",     fmt(L"%lld", static_cast<long long>(frames_.load())) });
    rows.push_back({ L"Dropped",    fmt(L"%lld", static_cast<long long>(dropped_.load())) });
    rows.push_back({ L"Decode",     fmt(L"%lld ms",
                                        static_cast<long long>(codec_time_.load())) });
    const int channels = audio_channels_.load();
    rows.push_back({ L"Audio", channels > 0
                                   ? fmt(L"%d ch  %d Hz", channels, audio_rate_.load())
                                   : std::wstring(L"none") });

    const float pad      = 12.0f;
    const float line_h   = 20.0f;
    const float gutter   = 12.0f;

    // Size both columns from what is actually in them, so a long source name
    // is not clipped and a short one does not leave the panel half empty.
    float label_w = 0.0f;
    float value_w = 0.0f;
    for (const auto& r : rows) {
        label_w = std::max(label_w, ctx.text_width(r.label, Font::Small));
        value_w = std::max(value_w, ctx.text_width(r.value, Font::Mono));
    }
    value_w = std::clamp(value_w, 96.0f, 300.0f);

    const float top       = kBarHeight + 14.0f;
    const float available = ctx.height() - top - kBarHeight - 14.0f;

    // Drop rows that will not fit rather than spill past the panel edge.
    size_t visible = rows.size();
    while (visible > 1 && pad * 2.0f + visible * line_h > available) --visible;

    const float w = pad * 2.0f + label_w + gutter + value_w;
    const float h = pad * 2.0f + visible * line_h;

    const D2D1_RECT_F panel = D2D1::RectF(14.0f, top, 14.0f + w, top + h);
    ctx.fill_rect(panel, gfx::rgb(0x0B0D10, 0.82f), 8.0f);
    ctx.stroke_rect(panel, gfx::rgb(0xFFFFFF, 0.08f), 1.0f, 8.0f);

    float y = panel.top + pad;
    for (size_t i = 0; i < visible; ++i) {
        ctx.text(ui::rect(panel.left + pad, y, label_w, line_h - 2.0f),
                 rows[i].label, Font::Small, theme().text_dim);
        ctx.text(ui::rect(panel.left + pad + label_w + gutter, y, value_w, line_h - 2.0f),
                 rows[i].value, Font::Mono, theme().text);
        y += line_h;
    }
}

void ViewerWindow::draw_metadata(ui::Ctx& ctx) {
    if (!show_metadata_) return;

    std::deque<std::wstring> lines;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        lines = metadata_;
    }

    const float w = 380.0f;
    const float h = 220.0f;
    const D2D1_RECT_F panel = D2D1::RectF(ctx.width() - w - 14.0f, kBarHeight + 14.0f,
                                          ctx.width() - 14.0f, kBarHeight + 14.0f + h);
    ctx.fill_rect(panel, gfx::rgb(0x0B0D10, 0.86f), 8.0f);
    ctx.stroke_rect(panel, gfx::rgb(0xFFFFFF, 0.08f), 1.0f, 8.0f);
    ctx.text(ui::rect(panel.left + 14.0f, panel.top + 8.0f, w - 28.0f, 18.0f),
             L"Metadata", Font::BodyBold, theme().text_dim);

    ctx.push_clip(ui::inset(panel, 2.0f, 30.0f));
    float y = panel.bottom - 20.0f;
    for (auto it = lines.rbegin(); it != lines.rend() && y > panel.top + 28.0f; ++it) {
        ctx.text(ui::rect(panel.left + 14.0f, y - 14.0f, w - 28.0f, 16.0f), *it,
                 Font::Mono, theme().text_dim);
        y -= 17.0f;
    }
    ctx.pop_clip();
}

void ViewerWindow::on_render(ui::Ctx& ctx) {
    // Video has already been drawn by on_render_video.
    draw_status(ctx);

    draw_meters(ctx);
    draw_stats(ctx);
    draw_metadata(ctx);

    const bool visible = chrome_visible() || ctx.popup_open();
    const float alpha = ctx.animate(900, visible ? 1.0f : 0.0f, 10.0f);
    draw_top_bar(ctx, alpha);
    draw_bottom_bar(ctx, alpha);
}
