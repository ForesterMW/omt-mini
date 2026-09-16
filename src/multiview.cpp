#include "multiview.h"
#include "settings.h"
#include "discovery.h"
#include "netinfo.h"
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

std::vector<D2D1_RECT_F> multiview_cells(int layout_index, float width, float height,
                                         float gap) {
    const auto& layouts = multiview_layouts();
    const auto& spec = layouts[std::clamp(layout_index, 0,
                                          static_cast<int>(layouts.size()) - 1)];

    const float cw = (width - gap * (spec.cols + 1)) / spec.cols;
    const float ch = (height - gap * (spec.rows + 1)) / spec.rows;

    auto cell_rect = [&](int col, int row, int span) {
        const float x = gap + col * (cw + gap);
        const float y = gap + row * (ch + gap);
        return D2D1::RectF(x, y, x + cw * span + gap * (span - 1),
                           y + ch * span + gap * (span - 1));
    };

    std::vector<D2D1_RECT_F> out;
    if (spec.hero > 0) {
        out.push_back(cell_rect(0, 0, spec.hero));
        for (int row = 0; row < spec.rows; ++row)
            for (int col = 0; col < spec.cols; ++col)
                if (col >= spec.hero || row >= spec.hero)
                    out.push_back(cell_rect(col, row, 1));
    } else {
        for (int row = 0; row < spec.rows; ++row)
            for (int col = 0; col < spec.cols; ++col)
                out.push_back(cell_rect(col, row, 1));
    }
    return out;
}

// ---- tile ---------------------------------------------------------------
void MultiviewTile::set_source(const std::string& address, bool preview) {
    std::lock_guard<std::mutex> control(control_mutex_);
    stop_locked();
    address_ = address;
    if (address.empty()) return;

    running_ = true;
    thread_ = std::thread([this, address, preview] { run(address, preview); });
}

void MultiviewTile::stop() {
    std::lock_guard<std::mutex> control(control_mutex_);
    stop_locked();
}

void MultiviewTile::stop_locked() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    connected_ = false;
    width_ = height_ = 0;
    gfx::DeviceLock lock;
    std::lock_guard<std::mutex> frame_lock(frame_mutex_);
    texture_.release();
}

void MultiviewTile::run(std::string address, bool preview) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const OMTReceiveFlags flags = preview ? OMTReceiveFlags_Preview : OMTReceiveFlags_None;

    omt::Receiver receiver;
    // A wall gets left up for hours, so a source that is not there yet is a
    // normal state to sit in rather than a reason to stop.
    while (running_) {
        if (receiver.open(address, OMTFrameType_Video, OMTPreferredVideoFormat_UYVYorBGRA,
                          flags))
            break;
        for (int i = 0; i < 40 && running_; ++i) Sleep(100);
    }
    if (!running_) { CoUninitialize(); return; }

    // A wall is not where anyone judges a picture, so never be the reason a
    // sender lifts its encoding for everyone else watching it.
    receiver.set_suggested_quality(OMTQuality_Low);

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
            gfx::DeviceLock lock;
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            texture_.upload(*frame);
        }
        width_  = frame->Width;
        height_ = frame->Height;
        aspect_ = frame->AspectRatio;
        connected_ = true;
        last_frame_ms = util::now_ms();

        // Read live: the window may have closed while the output kept this
        // tile running, and posting to a destroyed handle is pointless.
        if (HWND notify = multiview_engine().notify())
            PostMessageW(notify, WM_OMT_FRAME, 0, 0);
    }

    receiver.close();
    CoUninitialize();
}

bool MultiviewTile::draw(gfx::Surface& surface, const D2D1_RECT_F& dest) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!texture_.valid()) return false;
    texture_.draw(surface,
                  gfx::fit_rect(dest, texture_.width(), texture_.height(), aspect_.load()));
    return true;
}

bool MultiviewTile::draw_to(ID3D11RenderTargetView* target, const D2D1_RECT_F& dest) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!texture_.valid()) return false;
    texture_.draw_to(target,
                     gfx::fit_rect(dest, texture_.width(), texture_.height(), aspect_.load()));
    return true;
}

// ---- engine -------------------------------------------------------------
MultiviewEngine& MultiviewEngine::instance() {
    static MultiviewEngine engine;
    return engine;
}
MultiviewEngine& multiview_engine() { return MultiviewEngine::instance(); }

void MultiviewEngine::attach_window(HWND notify) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Settings& cfg = settings();
        if (!window_attached_ && !output_running_) {
            layout_   = std::clamp(cfg.multiview_layout, 0,
                                   static_cast<int>(multiview_layouts().size()) - 1);
            preview_  = cfg.multiview_preview;
            assigned_ = cfg.multiview_sources;
        }
        window_attached_ = true;
        notify_ = notify;
    }
    rebuild();
}

void MultiviewEngine::detach_window() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_attached_ = false;
        notify_ = nullptr;
    }
    rebuild();
}

void MultiviewEngine::set_output_running(bool running) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Settings& cfg = settings();
        if (running && !window_attached_ && !output_running_) {
            layout_   = std::clamp(cfg.multiview_layout, 0,
                                   static_cast<int>(multiview_layouts().size()) - 1);
            preview_  = cfg.multiview_preview;
            assigned_ = cfg.multiview_sources;
        }
        output_running_ = running;
    }
    rebuild();
}

void MultiviewEngine::retire(std::vector<std::shared_ptr<MultiviewTile>>& doomed) {
    for (auto& tile : doomed) if (tile) tile->stop();
    doomed.clear();
}

void MultiviewEngine::rebuild() {
    std::vector<std::shared_ptr<MultiviewTile>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!window_attached_ && !output_running_) {
            doomed.swap(tiles_);
        } else {
            const size_t count = static_cast<size_t>(multiview_tile_count(layout_));
            assigned_.resize(count);

            // Reuse the receivers already running so changing shape does not
            // tear every one down and reconnect them all.
            std::vector<std::shared_ptr<MultiviewTile>> next(count);
            for (size_t i = 0; i < count; ++i) {
                if (i < tiles_.size() && tiles_[i]) next[i] = std::move(tiles_[i]);
                else next[i] = std::make_shared<MultiviewTile>();
            }
            for (auto& leftover : tiles_) if (leftover) doomed.push_back(leftover);
            tiles_ = std::move(next);

            for (size_t i = 0; i < count; ++i) {
                if (tiles_[i]->address() != assigned_[i])
                    tiles_[i]->set_source(assigned_[i], preview_);
            }
        }
    }
    retire(doomed);
}

void MultiviewEngine::restart_tiles() {
    std::vector<std::shared_ptr<MultiviewTile>> current;
    std::vector<std::string> addresses;
    bool preview = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current   = tiles_;
        addresses = assigned_;
        preview   = preview_;
    }
    // Outside the lock: set_source stops the old receiver, which joins a thread.
    for (size_t i = 0; i < current.size() && i < addresses.size(); ++i)
        current[i]->set_source(addresses[i], preview);
}

MultiviewEngine::Snapshot MultiviewEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot out;
    out.layout   = layout_;
    out.tiles    = tiles_;
    out.assigned = assigned_;
    return out;
}

void MultiviewEngine::set_layout(int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        layout_ = std::clamp(index, 0, static_cast<int>(multiview_layouts().size()) - 1);
    }
    rebuild();
    save();
}

void MultiviewEngine::set_preview(bool preview) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (preview_ == preview) return;
        preview_ = preview;
    }
    // Every tile has to reconnect to change what it asks the sender for.
    restart_tiles();
    save();
}

size_t MultiviewEngine::tile_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tiles_.size();
}

std::string MultiviewEngine::source_at(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index < assigned_.size() ? assigned_[index] : std::string();
}

void MultiviewEngine::set_source(size_t index, const std::string& address) {
    std::shared_ptr<MultiviewTile> tile;
    bool preview = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= assigned_.size() || index >= tiles_.size()) return;
        assigned_[index] = address;
        tile    = tiles_[index];
        preview = preview_;
    }
    // Outside the lock: this joins the old receiver thread.
    tile->set_source(address, preview);
    save();
}

void MultiviewEngine::clear_sources() {
    std::vector<std::shared_ptr<MultiviewTile>> current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& address : assigned_) address.clear();
        current = tiles_;
    }
    for (auto& tile : current) tile->stop();
    save();
}

void MultiviewEngine::save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Settings& cfg = settings();
    cfg.multiview_layout  = layout_;
    cfg.multiview_preview = preview_;
    cfg.multiview_sources = assigned_;
    cfg.save();
}

// ---- output -------------------------------------------------------------
namespace {
MultiviewOutput g_output;
}
MultiviewOutput& multiview_output() { return g_output; }

MultiviewOutputStats MultiviewOutput::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool MultiviewOutput::start() {
    if (running_) return true;
    if (!omt::loaded() || !gfx::Device::ready()) return false;

    running_ = true;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = MultiviewOutputStats{};
        stats_.running = true;
    }
    multiview_engine().set_output_running(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void MultiviewOutput::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    multiview_engine().set_output_running(false);
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = MultiviewOutputStats{};
    util::logf("multiview output: stopped");
}

void MultiviewOutput::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const Settings cfg = settings();
    const int width  = std::clamp(cfg.multiview_output_width, 320, 3840);
    const int height = std::clamp(cfg.multiview_output_height, 180, 2160);
    const int fps    = std::clamp(cfg.multiview_output_fps, 1, 60);

    auto fail = [this](const char* why) {
        util::logf("multiview output: %s", why);
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.error = why;
        stats_.running = false;
    };

    gfx::OffscreenTarget target;
    {
        gfx::DeviceLock lock;
        if (!target.create(width, height)) {
            fail("could not create the compositing target");
            running_ = false;
            CoUninitialize();
            return;
        }
    }

    const auto ports_before = netinfo::listening_ports(cfg.port_start, cfg.port_end);

    omt::Sender sender;
    const std::string name = omt::safe_source_name(cfg.multiview_output_name, "Multiview");
    if (!sender.open(name, static_cast<OMTQuality>(cfg.capture_quality))) {
        fail("omt_send_create failed");
        running_ = false;
        CoUninitialize();
        return;
    }
    sender.set_sender_info("OMT Mini Multiview", "OMT Mini", OMTMINI_VERSION);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.width   = width;
        stats_.height  = height;
        stats_.address = sender.address();
    }
    util::logf("multiview output: '%s' %dx%d @%d", name.c_str(), width, height, fps);

    std::vector<uint8_t> pixels;
    const double frame_interval_ms = 1000.0 / fps;
    double next_frame_ms = static_cast<double>(util::now_ms());
    int64_t window_start = util::now_ms();
    int     window_frames = 0;

    while (running_) {
        const double now = static_cast<double>(util::now_ms());
        if (now < next_frame_ms) {
            Sleep(1);
            continue;
        }
        next_frame_ms = (now - next_frame_ms > frame_interval_ms * 4)
                      ? now + frame_interval_ms
                      : next_frame_ms + frame_interval_ms;

        // Taken before the device lock: acquiring the engine lock while
        // holding the device lock would close a cycle against a receiver
        // thread that wants the device lock to upload a frame.
        const auto view = multiview_engine().snapshot();
        const auto rects = multiview_cells(view.layout, static_cast<float>(width),
                                           static_cast<float>(height), kGap);

        {
            // The whole composite is one sequence of binds and draws, so it has
            // to be atomic against the interface drawing on the main thread.
            gfx::DeviceLock lock;

            target.clear(gfx::rgb(0x070809));
            for (size_t i = 0; i < rects.size() && i < view.tiles.size(); ++i)
                if (view.tiles[i]) view.tiles[i]->draw_to(target.rtv(), rects[i]);

            // Labels and tally, so a wall sent elsewhere is still readable.
            if (ID2D1DeviceContext* dc = target.d2d_begin()) {
                gfx::ComPtr<ID2D1SolidColorBrush> brush;
                dc->CreateSolidColorBrush(theme().text, brush.put());

                for (size_t i = 0; i < rects.size() && i < view.tiles.size(); ++i) {
                    auto& tile = view.tiles[i];
                    if (!tile) continue;
                    const std::string address =
                        i < view.assigned.size() ? view.assigned[i] : std::string();
                    if (address.empty()) continue;

                    const float strip_h = std::max(16.0f, height * 0.022f);
                    const D2D1_RECT_F strip = D2D1::RectF(rects[i].left,
                                                          rects[i].bottom - strip_h,
                                                          rects[i].right, rects[i].bottom);
                    brush->SetColor(gfx::rgb(0x000000, 0.55f));
                    dc->FillRectangle(&strip, brush.get());

                    brush->SetColor(theme().text);
                    const std::wstring label = util::widen(omt::short_name(address));
                    if (auto* format = gfx::text_format(gfx::Font::Small)) {
                        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        const D2D1_RECT_F text_rect =
                            D2D1::RectF(strip.left + 8.0f, strip.top, strip.right - 8.0f,
                                        strip.bottom);
                        dc->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                                      format, &text_rect, brush.get(),
                                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }

                    if (tile->program() || tile->preview_tally()) {
                        brush->SetColor(tile->program() ? theme().tally_pgm
                                                        : theme().tally_pvw);
                        const D2D1_RECT_F edge =
                            D2D1::RectF(rects[i].left + 1.5f, rects[i].top + 1.5f,
                                        rects[i].right - 1.5f, rects[i].bottom - 1.5f);
                        dc->DrawRectangle(&edge, brush.get(), 3.0f);
                    }
                }
                target.d2d_end();
            }

            if (!target.read_back(&pixels)) continue;
        }

        OMTMediaFrame frame{};
        frame.Type        = OMTFrameType_Video;
        frame.Timestamp   = util::now_omt_ticks();
        frame.Codec       = OMTCodec_BGRA;   // no alpha flag, so encoded as BGRX
        frame.Width       = width;
        frame.Height      = height;
        frame.Stride      = width * 4;
        frame.Flags       = OMTVideoFlags_None;
        frame.FrameRateN  = fps;
        frame.FrameRateD  = 1;
        frame.AspectRatio = static_cast<float>(width) / static_cast<float>(height);
        frame.ColorSpace  = OMTColorSpace_BT709;
        frame.Data        = pixels.data();
        frame.DataLength  = static_cast<int>(pixels.size());
        sender.send(&frame);
        ++window_frames;

        const int64_t elapsed = util::now_ms() - window_start;
        if (elapsed >= 1000) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.fps         = static_cast<float>(window_frames) * 1000.0f / elapsed;
            stats_.connections = sender.connections();
            stats_.address     = sender.address();
            stats_.running     = true;
            if (stats_.connect_url.empty()) {
                const int port = netinfo::port_opened_since(ports_before, cfg.port_start,
                                                            cfg.port_end);
                const std::string ip = netinfo::local_ipv4();
                if (port > 0 && !ip.empty()) {
                    stats_.connect_url = "omt://" + ip + ":" + std::to_string(port);
                    util::logf("multiview output: reachable at %s",
                               stats_.connect_url.c_str());
                }
            }
            window_frames = 0;
            window_start = util::now_ms();
        }
    }

    sender.close();
    {
        gfx::DeviceLock lock;
        target.destroy();
    }
    CoUninitialize();
}

// ---- window -------------------------------------------------------------
MultiviewWindow::~MultiviewWindow() {
    multiview_engine().detach_window();
}

void MultiviewWindow::open_or_focus() {
    if (hwnd()) {
        if (!visible()) show();
        else { ShowWindow(hwnd(), SW_RESTORE); SetForegroundWindow(hwnd()); }
        invalidate();
        return;
    }

    if (!create(L"OMT Mini Multiview", 1280, 720, true)) return;
    multiview_engine().attach_window(hwnd());
    last_activity_ms_ = util::now_ms();
    show();

    if (settings().multiview_fullscreen) toggle_fullscreen();
}

void MultiviewWindow::on_render_video() {
    const auto view = multiview_engine().snapshot();
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const auto rects = multiview_cells(view.layout, surface_.width() / scale,
                                       surface_.height() / scale, kGap);

    for (size_t i = 0; i < rects.size() && i < view.tiles.size(); ++i) {
        if (!view.tiles[i]) continue;
        const D2D1_RECT_F px = D2D1::RectF(rects[i].left * scale, rects[i].top * scale,
                                           rects[i].right * scale, rects[i].bottom * scale);
        view.tiles[i]->draw(surface_, px);
    }
}

void MultiviewWindow::draw_tile_overlay(ui::Ctx& ctx, const MultiviewEngine::Snapshot& view,
                                        size_t index, const D2D1_RECT_F& cell, float alpha) {
    if (index >= view.tiles.size() || !view.tiles[index]) return;
    auto& tile = view.tiles[index];

    const std::string address =
        index < view.assigned.size() ? view.assigned[index] : std::string();
    const bool has_source = !address.empty();

    if (!has_source) {
        ctx.fill_rect(cell, gfx::rgb(0x101216), 4.0f);
        ctx.stroke_rect(cell, theme().border, 1.0f, 4.0f);
        ctx.text(cell, L"empty", Font::Small, theme().text_dim, Align::Center);
    } else if (!tile->connected()) {
        ctx.fill_rect(cell, gfx::rgb(0x101216, 0.55f), 4.0f);
        ctx.text(cell, L"connecting", Font::Small, theme().text_dim, Align::Center);
    }

    // Tally borders the whole cell, which is how a wall is read. It stays put
    // when the labels fade: that is the one thing always worth seeing.
    if (tile->program() || tile->preview_tally()) {
        const auto colour = tile->program() ? theme().tally_pgm : theme().tally_pvw;
        ctx.stroke_rect(cell, colour, 3.0f, 4.0f);
    }

    if (has_source && alpha > 0.01f) {
        const float strip_h = 20.0f;
        const D2D1_RECT_F strip = D2D1::RectF(cell.left, cell.bottom - strip_h,
                                              cell.right, cell.bottom);
        ctx.fill_rect(strip, gfx::rgb(0x000000, 0.55f * alpha));

        auto label = theme().text;
        label.a *= alpha;
        ctx.text(ui::inset(strip, 8.0f, 0.0f), util::widen(omt::short_name(address)),
                 Font::Small, label);

        if (tile->width() > 0) {
            auto detail = theme().text_dim;
            detail.a *= alpha;
            ctx.text(ui::inset(strip, 8.0f, 0.0f),
                     fmt(L"%dx%d", tile->width(), tile->height()),
                     Font::Small, detail, Align::Right);
        }
    }

    // Choosing what goes in a cell only makes sense while the controls are up.
    if (alpha > 0.5f) {
        const auto id = static_cast<ui::Id>(400 + index * 2);
        if (ctx.clicked_area(id, cell)) {
            picking_ = static_cast<int>(index);
            open_picker_ = true;
            invalidate();
        }
    }
}

void MultiviewWindow::draw_toolbar(ui::Ctx& ctx, float alpha) {
    if (alpha <= 0.01f) return;

    auto& engine = multiview_engine();
    const float top = ctx.height() - kToolbarHeight;
    ctx.fill_rect(D2D1::RectF(0, top, ctx.width(), ctx.height()),
                  gfx::rgb(0x0B0D10, 0.80f * alpha));

    const float cy = top + kToolbarHeight * 0.5f;
    float x = 12.0f;

    std::vector<std::wstring> names;
    for (const auto& spec : multiview_layouts()) names.push_back(spec.name);
    int layout = engine.layout();
    if (ctx.dropdown(300, ui::rect(x, cy - 14.0f, 104.0f, 28.0f), names, &layout)) {
        pending_ = Pending::Layout;
        pending_value_ = layout;
        PostMessageW(hwnd(), WM_OMT_MVACTION, 0, 0);
    }
    x += 112.0f;

    bool preview = engine.preview();
    if (ctx.button(301, ui::rect(x, cy - 14.0f, 112.0f, 28.0f),
                   preview ? L"Preview feeds" : L"Full feeds",
                   preview ? ButtonStyle::Primary : ButtonStyle::Normal)) {
        pending_ = Pending::Preview;
        pending_value_ = preview ? 0 : 1;
        PostMessageW(hwnd(), WM_OMT_MVACTION, 0, 0);
    }
    x += 120.0f;

    // The output is the thing that makes this wall useful to other machines.
    const bool sending = multiview_output().running();
    if (ctx.button(305, ui::rect(x, cy - 14.0f, 128.0f, 28.0f),
                   sending ? L"Sending" : L"Send as source",
                   sending ? ButtonStyle::Primary : ButtonStyle::Normal)) {
        // Through the app, which defers it: stopping joins the compositing
        // thread and this is running inside a paint holding the device lock.
        App::instance().toggle_multiview_output();
    }
    x += 136.0f;

    if (sending) {
        const auto out = multiview_output().stats();
        ctx.text(ui::rect(x, top, 220.0f, kToolbarHeight),
                 fmt(L"%dx%d  %.0f fps  %d receiver%s", out.width, out.height, out.fps,
                     out.connections, out.connections == 1 ? L"" : L"s"),
                 Font::Small, theme().text_dim);
        ctx.request_redraw();
    }

    float rx = ctx.width() - 12.0f - 28.0f;
    if (ctx.icon_button(303, ui::rect(rx, cy - 14.0f, 28.0f, 28.0f), ui::Ctx::Glyph::Pop))
        toggle_fullscreen();
    rx -= 36.0f;

    if (ctx.button(304, ui::rect(rx - 60.0f, cy - 14.0f, 88.0f, 28.0f), L"Clear all",
                   ButtonStyle::Normal)) {
        pending_ = Pending::ClearAll;
        PostMessageW(hwnd(), WM_OMT_MVACTION, 0, 0);
    }
    rx -= 96.0f;

    bool autostart = settings().multiview_autostart;
    if (ctx.checkbox(302, ui::rect(rx - 150.0f, cy - 12.0f, 178.0f, 24.0f), &autostart,
                     L"Open at startup")) {
        settings().multiview_autostart = autostart;
        settings().save();
    }
}

void MultiviewWindow::on_render(ui::Ctx& ctx) {
    // Deliberately no background fill: on_render runs after the video, and
    // painting the window would cover every tile. The gaps come from
    // clear_colour().
    auto& engine = multiview_engine();
    // Taken once: it copies a vector of shared pointers, which is not something
    // to do per tile per frame.
    const auto view = engine.snapshot();

    const bool visible_chrome = chrome_visible() || ctx.popup_open() || picking_ >= 0;
    const float alpha = ctx.animate(900, visible_chrome ? 1.0f : 0.0f, 10.0f);

    const auto rects = multiview_cells(view.layout, ctx.width(), ctx.height(), kGap);
    for (size_t i = 0; i < rects.size(); ++i)
        draw_tile_overlay(ctx, view, i, rects[i], alpha);

    if (picking_ >= 0 && picking_ < static_cast<int>(rects.size())) {
        const auto sources = discovery().sources();
        std::vector<std::wstring> items{ L"None" };
        std::vector<std::string>  addresses{ "" };
        pick_index_ = 0;
        const std::string current =
            static_cast<size_t>(picking_) < view.assigned.size()
                ? view.assigned[static_cast<size_t>(picking_)] : std::string();
        for (const auto& source : sources) {
            addresses.push_back(source.address);
            items.push_back(util::widen(source.name) + L"   (" +
                            util::widen(source.host) + L")");
            if (source.address == current) pick_index_ = static_cast<int>(items.size()) - 1;
        }

        // Summoned by a click on the cell rather than on its own anchor, so it
        // has to be opened explicitly or it would close again this frame.
        if (open_picker_) {
            ctx.open_popup(350);
            open_picker_ = false;
        }

        const D2D1_RECT_F cell = rects[static_cast<size_t>(picking_)];
        const D2D1_RECT_F anchor =
            D2D1::RectF(cell.left + 8.0f, cell.top + 8.0f,
                        std::min(cell.right - 8.0f, cell.left + 280.0f), cell.top + 36.0f);
        int chosen = pick_index_;
        if (ctx.dropdown(350, anchor, items, &chosen)) {
            pending_ = Pending::Source;
            pending_index_ = picking_;
            pending_address_ =
                addresses[std::clamp(chosen, 0, static_cast<int>(addresses.size()) - 1)];
            PostMessageW(hwnd(), WM_OMT_MVACTION, 0, 0);
            picking_ = -1;
        } else if (!ctx.popup_open()) {
            picking_ = -1;
        }
    }

    draw_toolbar(ctx, alpha);
}

bool MultiviewWindow::chrome_visible() const {
    // A pointer that has left the window means nobody is reaching for a
    // control, so do not wait out the timeout.
    if (input_.mouse_x < 0.0f) return false;
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

        // Rounded corners would leave the desktop showing through four notches.
        set_window_rounded(hwnd(), false);
        SetWindowLongW(hwnd(), GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd(), HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
    } else {
        set_window_rounded(hwnd(), true);
        SetWindowLongW(hwnd(), GWL_STYLE, saved_style_);
        SetWindowPlacement(hwnd(), &saved_placement_);
        SetWindowPos(hwnd(), nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
    last_activity_ms_ = util::now_ms();
    settings().multiview_fullscreen = fullscreen_;
    settings().save();
    invalidate();
}

void MultiviewWindow::apply_pending() {
    const Pending what = pending_;
    pending_ = Pending::None;

    auto& engine = multiview_engine();
    switch (what) {
        case Pending::Source:
            engine.set_source(static_cast<size_t>(pending_index_), pending_address_);
            break;
        case Pending::Layout:  engine.set_layout(pending_value_); break;
        case Pending::Preview: engine.set_preview(pending_value_ != 0); break;
        case Pending::ClearAll: engine.clear_sources(); break;
        default: return;
    }
    invalidate();
}

bool MultiviewWindow::on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    switch (msg) {
        case WM_OMT_MVACTION:
            // Applied here, not during the paint that asked for it: these join
            // receiver threads, and a paint holds the device lock they need.
            apply_pending();
            result = 0;
            return true;

        case WM_MOUSELEAVE:
            invalidate();
            break;

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
    // The engine keeps running if the output is still sending, which is what
    // makes a headless wall possible.
    multiview_engine().detach_window();
    closed_ = true;
    destroy();
}
