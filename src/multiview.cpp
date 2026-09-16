#include "multiview.h"
#include "settings.h"
#include "discovery.h"
#include "app.h"

#include <objbase.h>
#include <algorithm>
#include <cstdio>

using gfx::theme;
using ui::Align;
using ui::ButtonStyle;
using ui::Font;

namespace {
constexpr int64_t kChromeTimeoutMs = 2600;
constexpr float   kToolbarHeight = 46.0f;
constexpr float   kGap = 3.0f;
constexpr UINT_PTR kChromeTimer = 1;

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

const std::vector<MultiviewLayout>& multiview_layouts() {
    static const std::vector<MultiviewLayout> v = {
        { L"2 x 2",   2, 2, 0 },
        { L"3 x 2",   3, 2, 0 },
        { L"3 x 3",   3, 3, 0 },
        { L"4 x 3",   4, 3, 0 },
        { L"4 x 4",   4, 4, 0 },
        { L"1 + 5",   3, 3, 2 },
        { L"1 + 7",   4, 4, 3 },
    };
    return v;
}

int multiview_tile_count(int layout_index) {
    const auto& layouts = multiview_layouts();
    if (layout_index < 0 || layout_index >= static_cast<int>(layouts.size()))
        layout_index = 0;
    const auto& spec = layouts[layout_index];
    if (spec.hero <= 0) return spec.cols * spec.rows;
    // The hero swallows hero*hero cells and gives back one.
    return spec.cols * spec.rows - spec.hero * spec.hero + 1;
}

// ---- tile ---------------------------------------------------------------
void MultiviewTile::set_source(const std::string& address, bool preview, HWND notify) {
    if (address_ == address && running_) return;
    stop();
    address_ = address;
    if (address.empty()) return;

    running_ = true;
    thread_ = std::thread([this, address, preview, notify] { run(address, preview, notify); });
}

void MultiviewTile::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
    } else if (thread_.joinable()) {
        thread_.join();
    }
    connected_ = false;
    width_ = height_ = 0;
    std::lock_guard<std::mutex> lock(frame_mutex_);
    texture_.release();
}

void MultiviewTile::run(std::string address, bool preview, HWND notify) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const OMTReceiveFlags flags = preview ? OMTReceiveFlags_Preview : OMTReceiveFlags_None;

    omt::Receiver receiver;
    // A wall gets left up for hours, so a source that is not there yet is a
    // normal state to sit in rather than a reason to stop.
    while (running_) {
        if (receiver.open(address, OMTFrameType_Video, OMTPreferredVideoFormat_UYVYorBGRA,
                          flags))
            break;
        util::logf("multiview: '%s' not available, retrying", address.c_str());
        for (int i = 0; i < 40 && running_; ++i) Sleep(100);
    }
    if (!running_) { CoUninitialize(); return; }

    // A wall is not where anyone judges quality, so never ask a sender to lift
    // its encoding on a multiview's account.
    receiver.set_suggested_quality(OMTQuality_Low);

    int64_t window_start = util::now_ms();
    int     window_frames = 0;
    int64_t last_frame_ms = 0;

    while (running_) {
        OMTTally tally{};
        if (receiver.get_tally(0, &tally)) {
            tally_pgm_ = tally.program != 0;
            tally_pvw_ = tally.preview != 0;
        }

        OMTMediaFrame* frame = receiver.receive(OMTFrameType_Video, 100);
        if (!frame) {
            if (util::now_ms() - last_frame_ms > 2000) connected_ = false;
            continue;
        }
        if (frame->Type != OMTFrameType_Video) continue;

        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            texture_.upload(*frame);
        }
        width_  = frame->Width;
        height_ = frame->Height;
        aspect_ = frame->AspectRatio;
        connected_ = true;
        last_frame_ms = util::now_ms();
        ++window_frames;

        if (notify) PostMessageW(notify, WM_OMT_FRAME, 0, 0);

        const int64_t elapsed = util::now_ms() - window_start;
        if (elapsed >= 1000) {
            fps_ = static_cast<float>(window_frames) * 1000.0f / elapsed;
            window_frames = 0;
            window_start = util::now_ms();
        }
    }

    receiver.close();
    CoUninitialize();
}

bool MultiviewTile::draw(gfx::Surface& surface, const D2D1_RECT_F& dest) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!texture_.valid()) return false;
    const D2D1_RECT_F fitted =
        gfx::fit_rect(dest, texture_.width(), texture_.height(), aspect_.load());
    texture_.draw(surface, fitted);
    return true;
}

// ---- window -------------------------------------------------------------
MultiviewWindow::MultiviewWindow() {
    const Settings& cfg = settings();
    layout_   = std::clamp(cfg.multiview_layout, 0,
                           static_cast<int>(multiview_layouts().size()) - 1);
    preview_  = cfg.multiview_preview;
    assigned_ = cfg.multiview_sources;
}

MultiviewWindow::~MultiviewWindow() {
    for (auto& tile : tiles_) if (tile) tile->stop();
    tiles_.clear();
}

void MultiviewWindow::open_or_focus() {
    if (hwnd()) {
        if (!visible()) show();
        else { ShowWindow(hwnd(), SW_RESTORE); SetForegroundWindow(hwnd()); }
        invalidate();
        return;
    }

    if (!create(L"OMT Mini Multiview", 1280, 720, true)) return;
    apply_layout();
    last_activity_ms_ = util::now_ms();
    show();

    if (settings().multiview_fullscreen) toggle_fullscreen();
}

void MultiviewWindow::apply_layout() {
    const int count = multiview_tile_count(layout_);
    assigned_.resize(static_cast<size_t>(count));

    // Reuse the tiles already running so changing shape does not tear down
    // every receiver and reconnect them all.
    std::vector<std::unique_ptr<MultiviewTile>> next(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (i < static_cast<int>(tiles_.size()) && tiles_[i]) next[i] = std::move(tiles_[i]);
        else next[i] = std::make_unique<MultiviewTile>();
    }
    // Anything left over is no longer on screen.
    for (auto& leftover : tiles_) if (leftover) leftover->stop();
    tiles_ = std::move(next);

    for (int i = 0; i < count; ++i)
        tiles_[i]->set_source(assigned_[i], preview_, hwnd());
}

void MultiviewWindow::save_state() const {
    Settings& cfg = settings();
    cfg.multiview_layout     = layout_;
    cfg.multiview_preview    = preview_;
    cfg.multiview_sources    = assigned_;
    cfg.multiview_fullscreen = fullscreen_;
    cfg.save();
}

std::vector<D2D1_RECT_F> MultiviewWindow::cells(float width, float height) const {
    const auto& layouts = multiview_layouts();
    const auto& spec = layouts[std::clamp(layout_, 0, static_cast<int>(layouts.size()) - 1)];

    const float cw = (width - kGap * (spec.cols + 1)) / spec.cols;
    const float ch = (height - kGap * (spec.rows + 1)) / spec.rows;

    auto cell_rect = [&](int col, int row, int span) {
        const float x = kGap + col * (cw + kGap);
        const float y = kGap + row * (ch + kGap);
        return D2D1::RectF(x, y, x + cw * span + kGap * (span - 1),
                           y + ch * span + kGap * (span - 1));
    };

    std::vector<D2D1_RECT_F> out;
    if (spec.hero > 0) {
        out.push_back(cell_rect(0, 0, spec.hero));
        for (int row = 0; row < spec.rows; ++row) {
            for (int col = 0; col < spec.cols; ++col) {
                if (col < spec.hero && row < spec.hero) continue;
                out.push_back(cell_rect(col, row, 1));
            }
        }
    } else {
        for (int row = 0; row < spec.rows; ++row)
            for (int col = 0; col < spec.cols; ++col)
                out.push_back(cell_rect(col, row, 1));
    }
    return out;
}

void MultiviewWindow::on_render_video() {
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const auto rects = cells(surface_.width() / scale, surface_.height() / scale);

    for (size_t i = 0; i < rects.size() && i < tiles_.size(); ++i) {
        if (!tiles_[i]) continue;
        const D2D1_RECT_F px = D2D1::RectF(rects[i].left * scale, rects[i].top * scale,
                                           rects[i].right * scale, rects[i].bottom * scale);
        tiles_[i]->draw(surface_, px);
    }
}

void MultiviewWindow::draw_tile_overlay(ui::Ctx& ctx, size_t index, const D2D1_RECT_F& cell) {
    auto& tile = tiles_[index];
    const bool has_source = !assigned_[index].empty();

    if (!has_source) {
        ctx.fill_rect(cell, gfx::rgb(0x101216), 4.0f);
        ctx.stroke_rect(cell, theme().border, 1.0f, 4.0f);
        ctx.text(cell, L"empty", Font::Small, theme().text_dim, Align::Center);
    } else if (!tile->connected()) {
        ctx.fill_rect(cell, gfx::rgb(0x101216, 0.55f), 4.0f);
        ctx.text(cell, L"connecting", Font::Small, theme().text_dim, Align::Center);
    }

    // Tally border: the whole cell edge, which is how a wall is read.
    if (tile->program() || tile->preview_tally()) {
        const auto colour = tile->program() ? theme().tally_pgm : theme().tally_pvw;
        ctx.stroke_rect(cell, colour, 3.0f, 4.0f);
    }

    // Name strip along the bottom of the cell.
    if (has_source) {
        const float strip_h = 20.0f;
        const D2D1_RECT_F strip = D2D1::RectF(cell.left, cell.bottom - strip_h,
                                              cell.right, cell.bottom);
        ctx.fill_rect(strip, gfx::rgb(0x000000, 0.55f));
        ctx.text(ui::inset(strip, 8.0f, 0.0f),
                 util::widen(omt::short_name(assigned_[index])), Font::Small,
                 theme().text);

        if (tile->width() > 0) {
            ctx.text(ui::inset(strip, 8.0f, 0.0f),
                     fmt(L"%dx%d", tile->width(), tile->height()),
                     Font::Small, theme().text_dim, Align::Right);
        }
    }

    // Clicking a cell chooses what goes in it.
    const auto id = static_cast<ui::Id>(400 + index * 2);
    if (ctx.clicked_area(id, cell)) {
        picking_ = static_cast<int>(index);
        open_picker_ = true;
        invalidate();
    }
}

void MultiviewWindow::draw_toolbar(ui::Ctx& ctx, float alpha) {
    if (alpha <= 0.01f) return;

    const float top = ctx.height() - kToolbarHeight;
    ctx.fill_rect(D2D1::RectF(0, top, ctx.width(), ctx.height()),
                  gfx::rgb(0x0B0D10, 0.80f * alpha));

    const float cy = top + kToolbarHeight * 0.5f;
    float x = 12.0f;

    ctx.text(ui::rect(x, top, 60.0f, kToolbarHeight), L"Layout", Font::Small,
             theme().text_dim);
    x += 62.0f;

    std::vector<std::wstring> names;
    for (const auto& spec : multiview_layouts()) names.push_back(spec.name);
    int layout = layout_;
    if (ctx.dropdown(300, ui::rect(x, cy - 14.0f, 110.0f, 28.0f), names, &layout)) {
        layout_ = layout;
        apply_layout();
        save_state();
    }
    x += 122.0f;

    if (ctx.button(301, ui::rect(x, cy - 14.0f, 116.0f, 28.0f),
                   preview_ ? L"Preview feeds" : L"Full feeds",
                   preview_ ? ButtonStyle::Primary : ButtonStyle::Normal)) {
        preview_ = !preview_;
        // Every tile has to reconnect to change what it is asking for.
        for (size_t i = 0; i < tiles_.size(); ++i) {
            tiles_[i]->stop();
            tiles_[i]->set_source(assigned_[i], preview_, hwnd());
        }
        save_state();
    }
    x += 128.0f;

    bool autostart = settings().multiview_autostart;
    if (ctx.checkbox(302, ui::rect(x, cy - 12.0f, 190.0f, 24.0f), &autostart,
                     L"Open at startup")) {
        settings().multiview_autostart = autostart;
        settings().save();
    }

    float rx = ctx.width() - 12.0f - 28.0f;
    if (ctx.icon_button(303, ui::rect(rx, cy - 14.0f, 28.0f, 28.0f), ui::Ctx::Glyph::Pop))
        toggle_fullscreen();
    rx -= 36.0f;

    if (ctx.button(304, ui::rect(rx - 60.0f, cy - 14.0f, 88.0f, 28.0f), L"Clear all",
                   ButtonStyle::Normal)) {
        for (size_t i = 0; i < tiles_.size(); ++i) {
            assigned_[i].clear();
            tiles_[i]->stop();
        }
        save_state();
    }
}

void MultiviewWindow::on_render(ui::Ctx& ctx) {
    // Deliberately no background fill here: on_render runs after the video has
    // been drawn, so painting the window would cover every tile. The background
    // between tiles comes from clear_colour().
    const auto rects = cells(ctx.width(), ctx.height());
    for (size_t i = 0; i < rects.size() && i < tiles_.size(); ++i)
        draw_tile_overlay(ctx, i, rects[i]);

    // Source picker for the cell that was clicked.
    if (picking_ >= 0 && picking_ < static_cast<int>(assigned_.size())) {
        const auto sources = discovery().sources();
        std::vector<std::wstring> items{ L"None" };
        std::vector<std::string>  addresses{ "" };
        pick_index_ = 0;
        for (const auto& source : sources) {
            addresses.push_back(source.address);
            items.push_back(util::widen(source.name) + L"   (" +
                            util::widen(source.host) + L")");
            if (source.address == assigned_[picking_])
                pick_index_ = static_cast<int>(items.size()) - 1;
        }

        const D2D1_RECT_F cell = rects[static_cast<size_t>(picking_)];
        const D2D1_RECT_F anchor = D2D1::RectF(cell.left + 8.0f, cell.top + 8.0f,
                                               std::min(cell.right - 8.0f,
                                                        cell.left + 280.0f),
                                               cell.top + 36.0f);
        // The list was summoned by a click on the cell, not on the list's own
        // anchor, so it has to be opened explicitly or it would close again on
        // this very frame.
        if (open_picker_) {
            ctx.open_popup(350);
            open_picker_ = false;
        }

        int chosen = pick_index_;
        if (ctx.dropdown(350, anchor, items, &chosen)) {
            const size_t index = static_cast<size_t>(picking_);
            assigned_[index] = addresses[std::clamp(chosen, 0,
                                                    static_cast<int>(addresses.size()) - 1)];
            tiles_[index]->set_source(assigned_[index], preview_, hwnd());
            save_state();
            picking_ = -1;
        } else if (!ctx.popup_open()) {
            // The list was dismissed without a choice.
            picking_ = -1;
        }
    }

    const bool visible_chrome = chrome_visible() || ctx.popup_open() || picking_ >= 0;
    const float alpha = ctx.animate(900, visible_chrome ? 1.0f : 0.0f, 10.0f);
    draw_toolbar(ctx, alpha);
}

bool MultiviewWindow::chrome_visible() const {
    return (util::now_ms() - last_activity_ms_) < kChromeTimeoutMs;
}

void MultiviewWindow::toggle_fullscreen() {
    if (!hwnd()) return;

    if (!fullscreen_) {
        saved_placement_.length = sizeof(saved_placement_);
        GetWindowPlacement(hwnd(), &saved_placement_);
        saved_style_ = GetWindowLongW(hwnd(), GWL_STYLE);

        HMONITOR monitor = MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(monitor, &mi)) return;

        SetWindowLongW(hwnd(), GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd(), HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd(), GWL_STYLE, saved_style_);
        SetWindowPlacement(hwnd(), &saved_placement_);
        SetWindowPos(hwnd(), nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
    last_activity_ms_ = util::now_ms();
    save_state();
    invalidate();
}

bool MultiviewWindow::on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    switch (msg) {
        case WM_MOUSEMOVE:
            last_activity_ms_ = util::now_ms();
            if (!timer_running_) {
                SetTimer(hwnd(), kChromeTimer, 250, nullptr);
                timer_running_ = true;
            }
            break;

        case WM_TIMER:
            if (wp == kChromeTimer) {
                invalidate();
                if (!chrome_visible() && picking_ < 0) {
                    KillTimer(hwnd(), kChromeTimer);
                    timer_running_ = false;
                }
                result = 0;
                return true;
            }
            break;

        case WM_SETCURSOR:
            if (fullscreen_ && LOWORD(lp) == HTCLIENT && !chrome_visible() && picking_ < 0) {
                SetCursor(nullptr);
                result = TRUE;
                return true;
            }
            break;

        case WM_KEYDOWN:
            last_activity_ms_ = util::now_ms();
            if (wp == VK_F11 || wp == 'F') { toggle_fullscreen(); result = 0; return true; }
            if (wp == VK_ESCAPE) {
                if (picking_ >= 0) { picking_ = -1; invalidate(); result = 0; return true; }
                if (fullscreen_)   { toggle_fullscreen(); result = 0; return true; }
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

void MultiviewWindow::on_closing() {
    save_state();
    for (auto& tile : tiles_) if (tile) tile->stop();
    tiles_.clear();
    closed_ = true;
    destroy();
}
