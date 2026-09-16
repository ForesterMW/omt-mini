// Application object: the tray icon, the menu, and the lifetime of every
// window OMT Mini opens.
#pragma once
#include "util.h"
#include "viewer.h"

#include <memory>
#include <vector>
#include <string>

class SourcesWindow;
class SettingsWindow;
class MultiviewWindow;

class App {
public:
    static App& instance();

    bool init(HINSTANCE instance);
    int  run();
    void quit();

    // ---- actions, shared by the tray menu and the windows ----
    void open_viewer(const std::string& address);
    void close_all_viewers();
    void show_sources();
    void show_settings(int tab = 0);
    void show_multiview();
    // Starts or stops the multiview as an OMT source. Independent of the
    // window, so it keeps sending with nothing open.
    //
    // Safe to call from inside a window's paint: the work is deferred to the
    // message loop, because stopping joins the compositing thread and a paint
    // holds the device lock that thread needs to shut down.
    void toggle_multiview_output();
    bool toggle_desktop_capture();
    bool toggle_webcam();
    // Requests the update install. Safe to call from inside a window's paint:
    // the work is deferred to the message loop. Only ever reached from a
    // button press; nothing in OMT Mini installs an update by itself.
    void install_update();
    void notify(const std::wstring& title, const std::wstring& message);

    size_t viewer_count() const { return viewers_.size(); }
    HWND   message_window() const { return hwnd_; }

    // Start at login is stored in the per-user Run key.
    static bool run_at_login();
    static void set_run_at_login(bool on);

private:
    App() = default;

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    bool create_message_window(HINSTANCE instance);
    bool add_tray_icon();
    void remove_tray_icon();
    void update_tray_tip();
    void show_tray_menu();
    void prune_viewers();
    void do_install_update();
    void do_toggle_multiview_output();

    HINSTANCE    instance_ = nullptr;
    HWND         hwnd_ = nullptr;
    HICON        icon_ = nullptr;
    bool         tray_added_ = false;
    bool         quitting_ = false;

    std::vector<std::unique_ptr<ViewerWindow>> viewers_;
    std::unique_ptr<SourcesWindow>  sources_window_;
    std::unique_ptr<SettingsWindow> settings_window_;
    std::unique_ptr<MultiviewWindow> multiview_window_;
    std::vector<std::string>        menu_sources_;
    bool                            update_announced_ = false;
};
