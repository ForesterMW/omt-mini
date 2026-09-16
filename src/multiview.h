// Multiview: one picture built from many sources.
//
// Split three ways so it can run with a window, without one, or both at once:
//
//   MultiviewEngine  owns the receivers and the layout. Shared.
//   MultiviewWindow  shows the engine on screen.
//   MultiviewOutput  composes the engine into an OMT sender, on its own
//                    thread, with no window involved.
//
// The output is what lets a machine build a wall and hand it to everyone else
// as a single source, and it is why the engine is not owned by the window.
#pragma once
#include "window.h"
#include "omt.h"

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

struct MultiviewLayout {
    const wchar_t* name;
    int cols;
    int rows;
    int hero;        // span of the large tile, 0 for an even grid
};
const std::vector<MultiviewLayout>& multiview_layouts();
int multiview_tile_count(int layout_index);
// Cell rectangles for a layout inside a region of the given size.
std::vector<D2D1_RECT_F> multiview_cells(int layout_index, float width, float height,
                                         float gap);

// One cell: its own receiver thread, its own texture.
class MultiviewTile {
public:
    ~MultiviewTile() { stop(); }

    void set_source(const std::string& address, bool preview);
    void stop();

    const std::string& address() const { return address_; }
    bool  connected() const { return connected_; }
    bool  program() const { return tally_pgm_; }
    bool  preview_tally() const { return tally_pvw_; }
    int   width() const { return width_; }
    int   height() const { return height_; }
    float aspect() const { return aspect_; }

    // Both take the caller's device lock as given.
    bool draw(gfx::Surface& surface, const D2D1_RECT_F& dest);
    bool draw_to(ID3D11RenderTargetView* target, const D2D1_RECT_F& dest);

private:
    void run(std::string address, bool preview);
    void stop_locked();

    // ---- 3. serialises set_source against stop, so two callers cannot both
    // try to join the same receiver thread, which is undefined behaviour.
    std::mutex         control_mutex_;
    std::string        address_;
    std::thread        thread_;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  connected_{false};
    std::atomic<bool>  tally_pgm_{false};
    std::atomic<bool>  tally_pvw_{false};
    std::atomic<int>   width_{0}, height_{0};
    std::atomic<float> aspect_{0.0f};

    std::mutex        frame_mutex_;
    gfx::VideoTexture texture_;
};

// Shared state: the receivers, the shape, and what is in each cell.
class MultiviewEngine {
public:
    static MultiviewEngine& instance();

    // Tiles run while anything is attached, and stop when nothing is.
    void attach_window(HWND notify);
    void detach_window();
    void set_output_running(bool running);
    bool active() const { return window_attached_ || output_running_; }

    // Where tiles post a repaint. Read live rather than captured, so a tile
    // outliving its window does not post to a destroyed handle.
    HWND notify() const { return notify_.load(); }

    int  layout() const { return layout_; }
    void set_layout(int index);
    bool preview() const { return preview_; }
    void set_preview(bool preview);

    // A consistent view of the tiles, taken without the device lock held.
    // Shared pointers so a layout change during a composite cannot free a tile
    // out from under it; a stopped tile simply draws nothing.
    struct Snapshot {
        int layout = 0;
        std::vector<std::shared_ptr<MultiviewTile>> tiles;
        std::vector<std::string> assigned;
    };
    Snapshot snapshot() const;

    size_t tile_count() const;
    std::string source_at(size_t index) const;
    void set_source(size_t index, const std::string& address);
    void clear_sources();

    void save() const;

private:
    MultiviewEngine() = default;
    void rebuild();
    void restart_tiles();
    // Tiles are stopped after the lock is released: stopping joins a receiver
    // thread, and that thread may be waiting on the device lock, which the
    // caller can already be holding.
    static void retire(std::vector<std::shared_ptr<MultiviewTile>>& doomed);

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<MultiviewTile>> tiles_;
    std::vector<std::string> assigned_;
    int  layout_ = 0;
    bool preview_ = true;
    bool window_attached_ = false;
    bool output_running_ = false;
    std::atomic<HWND> notify_{nullptr};
};

MultiviewEngine& multiview_engine();

struct MultiviewOutputStats {
    bool        running = false;
    int         width = 0, height = 0;
    float       fps = 0.0f;
    int         connections = 0;
    std::string address;
    std::string connect_url;
    std::string error;
};

// Composes the engine into an OMT source. Runs with or without the window.
class MultiviewOutput {
public:
    ~MultiviewOutput() { stop(); }

    bool start();
    void stop();
    bool running() const { return running_; }
    MultiviewOutputStats stats() const;

private:
    void run();

    std::thread        thread_;
    std::atomic<bool>  running_{false};
    mutable std::mutex stats_mutex_;
    MultiviewOutputStats stats_;
};

MultiviewOutput& multiview_output();

class MultiviewWindow : public Window {
public:
    ~MultiviewWindow() override;

    void open_or_focus();
    bool closed() const { return closed_; }

protected:
    void on_render(ui::Ctx& ctx) override;
    void on_render_video() override;
    D2D1_COLOR_F clear_colour() const override { return gfx::rgb(0x070809); }
    bool on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;
    void on_closing() override;
    const wchar_t* class_name() const override { return L"OMTMiniMultiview"; }

private:
    void toggle_fullscreen();
    bool chrome_visible() const;
    void draw_tile_overlay(ui::Ctx& ctx, const MultiviewEngine::Snapshot& view,
                           size_t index, const D2D1_RECT_F& cell, float alpha);
    void draw_toolbar(ui::Ctx& ctx, float alpha);

    // Engine changes join receiver threads, and a paint holds the device lock
    // that those threads need, so a change asked for during a paint is applied
    // from the message loop instead.
    enum class Pending { None, Source, Layout, Preview, ClearAll };
    Pending      pending_ = Pending::None;
    int          pending_index_ = 0;
    int          pending_value_ = 0;
    std::string  pending_address_;
    void apply_pending();

    bool  fullscreen_ = false;
    bool  closed_ = false;
    int   picking_ = -1;
    bool  open_picker_ = false;
    int   pick_index_ = 0;
    int64_t last_activity_ms_ = 0;
    bool  timer_running_ = false;

    WINDOWPLACEMENT saved_placement_{};
    LONG            saved_style_ = 0;
};
