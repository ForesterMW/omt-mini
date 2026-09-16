// Multiview: one window showing many sources at once.
//
// Tiles receive in OMT's preview mode by default, which is the sender's own
// one eighth resolution stream. Sixteen of those cost a fraction of sixteen
// full feeds, which is the difference between a usable wall and a melted
// network.
#pragma once
#include "window.h"
#include "omt.h"

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

// Grid shapes, including the classic hero plus row arrangements.
struct MultiviewLayout {
    const wchar_t* name;
    int cols;
    int rows;
    int hero;        // span of the large tile, 0 for an even grid
};
const std::vector<MultiviewLayout>& multiview_layouts();
int multiview_tile_count(int layout_index);

// One cell: its own receiver thread, its own texture.
class MultiviewTile {
public:
    ~MultiviewTile() { stop(); }

    void set_source(const std::string& address, bool preview, HWND notify);
    void stop();

    const std::string& address() const { return address_; }
    bool  connected() const { return connected_; }
    bool  program() const { return tally_pgm_; }
    bool  preview_tally() const { return tally_pvw_; }
    int   width() const { return width_; }
    int   height() const { return height_; }
    float aspect() const { return aspect_; }
    float fps() const { return fps_; }

    // Draws into dest, in pixels. Returns false when there is nothing yet.
    bool draw(gfx::Surface& surface, const D2D1_RECT_F& dest);

private:
    void run(std::string address, bool preview, HWND notify);

    std::string        address_;
    std::thread        thread_;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  connected_{false};
    std::atomic<bool>  tally_pgm_{false};
    std::atomic<bool>  tally_pvw_{false};
    std::atomic<int>   width_{0}, height_{0};
    std::atomic<float> aspect_{0.0f};
    std::atomic<float> fps_{0.0f};

    std::mutex        frame_mutex_;
    gfx::VideoTexture texture_;
};

class MultiviewWindow : public Window {
public:
    MultiviewWindow();
    ~MultiviewWindow() override;

    void open_or_focus();
    bool closed() const { return closed_; }

protected:
    void on_render(ui::Ctx& ctx) override;
    void on_render_video() override;
    bool on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;
    void on_closing() override;
    const wchar_t* class_name() const override { return L"OMTMiniMultiview"; }

private:
    std::vector<D2D1_RECT_F> cells(float width, float height) const;
    void apply_layout();
    void save_state() const;
    void toggle_fullscreen();
    bool chrome_visible() const;
    void draw_tile_overlay(ui::Ctx& ctx, size_t index, const D2D1_RECT_F& cell);
    void draw_toolbar(ui::Ctx& ctx, float alpha);

    std::vector<std::unique_ptr<MultiviewTile>> tiles_;
    std::vector<std::string> assigned_;

    int   layout_ = 0;
    bool  preview_ = true;
    bool  fullscreen_ = false;
    bool  closed_ = false;
    int   picking_ = -1;          // tile whose source list is open
    bool  open_picker_ = false;   // the list still has to be summoned
    int   pick_index_ = 0;
    int64_t last_activity_ms_ = 0;
    bool  timer_running_ = false;

    WINDOWPLACEMENT saved_placement_{};
    LONG            saved_style_ = 0;
};
