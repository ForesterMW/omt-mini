// Base window: swap chain, immediate mode UI context, input translation and
// DPI handling.
//
// Windows repaint on demand. A window that nothing is happening in receives no
// WM_PAINT and burns no CPU; viewers push a repaint when a video frame lands.
#pragma once
#include "gfx.h"
#include "ui.h"

#include <string>

// Posted to a window to ask for exactly one more frame.
#define WM_OMT_REDRAW   (WM_APP + 10)
#define WM_OMT_FRAME    (WM_APP + 11)
#define WM_OMT_SOURCES  (WM_APP + 12)
#define WM_OMT_TRAY     (WM_APP + 13)
#define WM_OMT_SHOWMAIN (WM_APP + 14)
#define WM_OMT_UPDATE   (WM_APP + 15)
#define WM_OMT_INSTALL  (WM_APP + 16)
#define WM_OMT_SCAN     (WM_APP + 17)
#define WM_OMT_MVACTION (WM_APP + 18)

class Window {
public:
    virtual ~Window();

    bool create(const std::wstring& title, int width, int height,
                bool resizable = true, HWND owner = nullptr);
    void destroy();

    void show(bool activate = true);
    void hide();
    bool visible() const;
    void invalidate();                 // schedule one repaint
    void set_always_on_top(bool on);
    void center_on_cursor();

    HWND hwnd() const { return hwnd_; }
    float dpi_scale() const { return dpi_ / 96.0f; }

protected:
    // Called to draw the window contents. Coordinates are DIPs.
    virtual void on_render(ui::Ctx& ctx) = 0;
    // Direct3D pass, run after the clear and before the Direct2D batch opens.
    // Issuing D3D draws while D2D has a batch open on the same target is not
    // defined, so video rendering belongs here rather than in on_render.
    virtual void on_render_video() {}
    // Background the frame is cleared to before any video is drawn. A window
    // that shows video must not paint its own background in on_render, which
    // runs afterwards and would cover it.
    virtual D2D1_COLOR_F clear_colour() const { return gfx::theme().bg; }
    // Return true to swallow the message.
    virtual bool on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) { return false; }
    virtual void on_resize(UINT width, UINT height) {}
    virtual void on_closing() {}
    virtual bool on_close_request() { return true; }   // false keeps the window open
    virtual const wchar_t* class_name() const { return L"OMTMiniWindow"; }

    // Draws video under the UI pass. Rect is in DIPs.
    void render_frame();

    gfx::Surface  surface_;
    ui::Ctx       ctx_;
    ui::Input     input_;
    HWND          hwnd_ = nullptr;
    UINT          dpi_ = 96;
    bool          in_render_ = false;

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void apply_dark_titlebar();
    void update_mouse(LPARAM lp);

    bool tracking_mouse_ = false;
};

// Applies the dark immersive title bar and rounded corners where supported.
void apply_window_theme(HWND hwnd);
// Windows 11 rounds window corners, which in full screen leaves the desktop
// showing through four notches. Square them off while covering the screen.
void set_window_rounded(HWND hwnd, bool rounded);
