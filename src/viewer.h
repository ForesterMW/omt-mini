// A live viewer for one OMT source. Any number can be open at once; each owns
// its own receiver thread, GPU texture and audio monitor.
#pragma once
#include "window.h"
#include "omt.h"
#include "audio.h"

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>

class ViewerWindow : public Window {
public:
    explicit ViewerWindow(const std::string& address);
    ~ViewerWindow() override;

    bool open();
    const std::string& address() const { return address_; }
    bool closed() const { return closed_; }

protected:
    void on_render(ui::Ctx& ctx) override;
    void on_render_video() override;
    bool on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;
    void on_closing() override;
    const wchar_t* class_name() const override { return L"OMTMiniViewer"; }

private:
    void receive_loop();
    void draw_top_bar(ui::Ctx& ctx, float alpha);
    void draw_bottom_bar(ui::Ctx& ctx, float alpha);
    void draw_meters(ui::Ctx& ctx);
    void draw_stats(ui::Ctx& ctx);
    void draw_status(ui::Ctx& ctx);
    void draw_metadata(ui::Ctx& ctx);
    void toggle_fullscreen();
    void apply_flags();
    bool chrome_visible() const;

    std::string        address_;
    std::wstring       title_;
    std::thread        thread_;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  closed_{false};

    omt::Receiver      receiver_;
    AudioMonitor       audio_;

    // Guards texture_ between the receive thread and the render thread.
    std::mutex         frame_mutex_;
    gfx::VideoTexture  texture_;

    std::atomic<bool>  connected_{false};
    std::atomic<int>   frame_w_{0}, frame_h_{0};
    std::atomic<float> aspect_{0.0f};
    std::atomic<int>   codec_{0};
    std::atomic<bool>  interlaced_{false};
    std::atomic<bool>  unsupported_format_{false};
    std::atomic<float> fps_{0.0f};
    std::atomic<float> mbps_{0.0f};
    std::atomic<int>   audio_channels_{0};
    std::atomic<int>   audio_rate_{0};
    std::atomic<int64_t> frames_{0};
    std::atomic<int64_t> dropped_{0};
    std::atomic<int64_t> codec_time_{0};
    std::atomic<int64_t> last_frame_ms_{0};
    // Tally as the sender reports it, across every receiver connected to it.
    // OMT Mini never asserts tally of its own: it is a monitor, and a monitor
    // that told a source it was on air simply because someone opened a window
    // on it would be actively harmful.
    std::atomic<bool>  agg_pgm_{false};
    std::atomic<bool>  agg_pvw_{false};
    std::atomic<bool>  tally_known_{false};

    std::mutex               meta_mutex_;
    std::deque<std::wstring> metadata_;
    std::wstring             sender_product_;

    // UI state
    bool  fullscreen_ = false;
    bool  show_stats_ = false;
    bool  show_metadata_ = false;
    bool  on_top_ = false;
    bool  low_bandwidth_ = false;
    bool  muted_ = false;
    float volume_ = 1.0f;
    int   quality_index_ = 0;
    int64_t last_activity_ms_ = 0;
    bool  timer_running_ = false;

    WINDOWPLACEMENT saved_placement_{};
    LONG            saved_style_ = 0;
};
