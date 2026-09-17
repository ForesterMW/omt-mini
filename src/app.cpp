#include "app.h"
#include "sourceswin.h"
#include "settingswin.h"
#include "multiview.h"
#include "discovery.h"
#include "capture.h"
#include "webcam.h"
#include "settings.h"
#include "update.h"
#include "announce.h"
#include "webui.h"
#include "scan.h"
#include "omt.h"
#include "gfx.h"

#include <shellapi.h>
#include <algorithm>

namespace {
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kIdSourcesFirst   = 2000,   // 2000 + index
    kIdSourcesLast    = 2499,
    kIdShowSources    = 3001,
    kIdSettings       = 3002,
    kIdDesktopCapture = 3003,
    kIdWebcam         = 3004,
    kIdCloseViewers   = 3005,
    kIdOpenLog        = 3006,
    kIdUpdate         = 3007,
    kIdMultiview      = 3008,
    kIdMultiviewOut   = 3009,
    kIdExit           = 3010,
};

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"OMT Mini";
} // namespace

App& App::instance() {
    static App app;
    return app;
}

bool App::run_at_login() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf), type = 0;
    const LONG rc = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                     reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_SZ && buf[0] != L'\0';
}

void App::set_run_at_login(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    if (on) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"";
        quoted += path;
        quoted += L"\" --tray";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
    util::logf("app: run at login = %d", on ? 1 : 0);
}

bool App::create_message_window(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &App::wnd_proc;
    wc.hInstance     = instance;
    wc.lpszClassName = L"OMTMiniTray";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, L"OMTMiniTray", L"OMT Mini", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, instance, nullptr);
    return hwnd_ != nullptr;
}

bool App::add_tray_icon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd_;
    nid.uID              = kTrayId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_OMT_TRAY;
    nid.hIcon            = icon_;
    lstrcpynW(nid.szTip, L"OMT Mini", ARRAYSIZE(nid.szTip));

    tray_added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (tray_added_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        util::logf("app: Shell_NotifyIcon add failed err=%lu", GetLastError());
    }
    return tray_added_;
}

void App::remove_tray_icon() {
    if (!tray_added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    tray_added_ = false;
}

void App::update_tray_tip() {
    if (!tray_added_) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = kTrayId;
    nid.uFlags = NIF_TIP;

    wchar_t tip[128];
    const size_t sources = discovery().count();
    _snwprintf(tip, ARRAYSIZE(tip), L"OMT Mini\n%zu source%s  -  %zu viewer%s",
               sources, sources == 1 ? L"" : L"s",
               viewers_.size(), viewers_.size() == 1 ? L"" : L"s");
    tip[ARRAYSIZE(tip) - 1] = L'\0';
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::notify(const std::wstring& title, const std::wstring& message) {
    if (!tray_added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = kTrayId;
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, message.c_str(), ARRAYSIZE(nid.szInfo));
    nid.dwInfoFlags = NIIF_NONE;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::show_tray_menu() {
    prune_viewers();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // ---- discovered sources ----
    const auto sources = discovery().sources();
    menu_sources_.clear();

    HMENU sources_menu = CreatePopupMenu();
    if (sources.empty()) {
        AppendMenuW(sources_menu, MF_STRING | MF_GRAYED, 0, L"No sources found");
    } else {
        for (size_t i = 0; i < sources.size() && i < 200; ++i) {
            std::wstring label = util::widen(sources[i].name);
            if (!sources[i].host.empty())
                label += L"   (" + util::widen(sources[i].host) + L")";
            AppendMenuW(sources_menu, MF_STRING,
                        kIdSourcesFirst + i, label.c_str());
            menu_sources_.push_back(sources[i].address);
        }
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sources_menu),
                L"Open viewer");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdShowSources, L"Sources...");
    AppendMenuW(menu, MF_STRING, kIdMultiview, L"Multiview");
    if (!viewers_.empty()) {
        wchar_t label[64];
        _snwprintf(label, 64, L"Close all viewers (%zu)", viewers_.size());
        AppendMenuW(menu, MF_STRING, kIdCloseViewers, label);
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (desktop_capture().running() ? MF_CHECKED : 0),
                kIdDesktopCapture, L"Desktop capture");
    AppendMenuW(menu, MF_STRING | (webcam().running() ? MF_CHECKED : 0),
                kIdWebcam, L"Webcam output");
    AppendMenuW(menu, MF_STRING | (multiview_output().running() ? MF_CHECKED : 0),
                kIdMultiviewOut, L"Multiview output");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdSettings, L"Settings...");
    if (updater().update_available()) {
        const UpdateInfo up = updater().info();
        wchar_t label[96];
        _snwprintf(label, 96, L"Update to %hs...", up.latest_version.c_str());
        label[95] = L'\0';
        AppendMenuW(menu, MF_STRING, kIdUpdate, label);
    }
    AppendMenuW(menu, MF_STRING, kIdOpenLog, L"Open log folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdExit, L"Exit OMT Mini");

    POINT pt{};
    GetCursorPos(&pt);
    // Required so the menu dismisses when the user clicks elsewhere.
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

void App::prune_viewers() {
    const size_t before = viewers_.size();
    viewers_.erase(std::remove_if(viewers_.begin(), viewers_.end(),
                                  [](const std::unique_ptr<ViewerWindow>& v) {
                                      return !v || v->closed();
                                  }),
                   viewers_.end());
    if (viewers_.size() != before) update_tray_tip();
}

void App::open_viewer(const std::string& address) {
    if (address.empty()) return;

    auto viewer = std::make_unique<ViewerWindow>(address);
    if (!viewer->open()) {
        util::logf("app: viewer failed to open for '%s'", address.c_str());
        MessageBoxW(dialog_owner(), L"The viewer window could not be created.",
                    L"OMT Mini", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        return;
    }
    viewers_.push_back(std::move(viewer));
    update_tray_tip();
}

void App::close_all_viewers() {
    for (auto& v : viewers_)
        if (v && !v->closed()) PostMessageW(v->hwnd(), WM_CLOSE, 0, 0);
}

void App::show_sources() {
    if (!sources_window_) sources_window_ = std::make_unique<SourcesWindow>();
    sources_window_->open_or_focus();
}

void App::toggle_multiview_output() {
    // Deferred, always. Stopping joins the compositing thread, which needs the
    // device lock to release its target, and every button that calls this is
    // drawn inside a paint that is already holding that lock.
    if (hwnd_) PostMessageW(hwnd_, WM_OMT_MVOUTPUT, 0, 0);
}

void App::do_toggle_multiview_output() {
    if (multiview_output().running()) {
        multiview_output().stop();
        settings().multiview_output_enabled = false;
        settings().save();
    } else if (multiview_output().start()) {
        settings().multiview_output_enabled = true;
        settings().save();
    } else {
        MessageBoxW(dialog_owner(), L"The multiview output could not start. See the log.",
                    L"OMT Mini", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }

    if (sources_window_)   sources_window_->invalidate();
    if (settings_window_)  settings_window_->invalidate();
    if (multiview_window_) multiview_window_->invalidate();
}

void App::show_multiview() {
    if (multiview_window_ && multiview_window_->closed()) multiview_window_.reset();
    if (!multiview_window_) multiview_window_ = std::make_unique<MultiviewWindow>();
    multiview_window_->open_or_focus();
}

void App::show_settings(int tab) {
    if (!settings_window_) settings_window_ = std::make_unique<SettingsWindow>();
    settings_window_->open_or_focus(tab);
}

bool App::toggle_desktop_capture() {
    if (desktop_capture().running()) {
        desktop_capture().stop();
        return false;
    }
    if (!desktop_capture().start()) {
        MessageBoxW(dialog_owner(),
                    L"Desktop capture could not start. See the log for details.",
                    L"OMT Mini", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        return false;
    }
    return true;
}

bool App::toggle_webcam() {
    if (webcam().running()) {
        webcam().stop();
        return false;
    }

    const std::string source = settings().webcam_source;
    if (source.empty()) {
        MessageBoxW(dialog_owner(),
                    L"Choose a source for the webcam output in Settings first.",
                    L"OMT Mini", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        show_settings(settings_tab::Webcam);
        return false;
    }
    if (!WebcamOutput::filter_registered()) {
        const int answer = MessageBoxW(dialog_owner(),
            L"The OMT Mini virtual camera is not registered yet.\n\n"
            L"Register it now for this user? No administrator rights are needed.",
            L"OMT Mini", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
        if (answer != IDYES) return false;

        std::wstring message;
        if (!WebcamOutput::register_filter(true, &message)) {
            MessageBoxW(dialog_owner(), message.c_str(), L"OMT Mini", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
            return false;
        }
    }
    return webcam().start(source);
}

HWND App::dialog_owner() const {
    // An owned dialog stays above its owner and gets grouped with it on the
    // taskbar. Without one, and without asking for the foreground, a
    // confirmation can open behind the window that asked for it, which reads
    // as the button having done nothing at all.
    if (settings_window_ && settings_window_->visible()) return settings_window_->hwnd();
    if (sources_window_ && sources_window_->visible())   return sources_window_->hwnd();
    if (multiview_window_ && !multiview_window_->closed() && multiview_window_->visible())
        return multiview_window_->hwnd();
    return nullptr;
}

void App::install_update() {
    // Called from a button, which means from inside that window's paint. Doing
    // the work here would tear the window down while its own render is still
    // on the stack, so it is deferred to the tray window's message loop.
    util::logf("update: install requested");
    if (hwnd_) PostMessageW(hwnd_, WM_OMT_INSTALL, 0, 0);
}

void App::do_install_update() {
    const UpdateInfo up = updater().info();
    if (up.state != UpdateState::ReadyToInstall || up.downloaded_path.empty()) {
        // Logged rather than silent: an install that appears to do nothing is
        // impossible to diagnose after the fact otherwise.
        util::logf("update: install ignored (state=%d, path=%s)",
                   static_cast<int>(up.state),
                   up.downloaded_path.empty() ? "none" : "set");
        return;
    }

    if (!viewers_.empty() || desktop_capture().running() || webcam().running()) {
        const int answer = MessageBoxW(dialog_owner(),
            L"Installing will close the viewers and stop desktop capture and the "
            L"webcam output.\n\nContinue?",
            L"OMT Mini", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
        if (answer != IDYES) {
            util::logf("update: install declined at the confirmation");
            return;
        }
    }

    util::logf("update: launching installer %s", util::narrow(up.downloaded_path).c_str());

    // Close the windows first so it is visibly acting on the press, then hand
    // over. The installer waits for this process to release its instance mutex
    // before it touches any files.
    if (settings_window_) settings_window_->hide();
    if (sources_window_)  sources_window_->hide();

    const HINSTANCE result = ShellExecuteW(nullptr, L"open", up.downloaded_path.c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        util::logf("update: ShellExecute failed err=%lu", GetLastError());
        MessageBoxW(dialog_owner(), L"The installer could not be started.", L"OMT Mini",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        if (settings_window_) settings_window_->show();
        return;
    }
    quit();
}

// ---- control panel ------------------------------------------------------
std::vector<WebWindow> App::web_windows() const {
    std::lock_guard<std::mutex> lock(web_mutex_);
    return web_snapshot_;
}

void App::refresh_web_snapshot() {
    // Interface thread only. This walks the live viewers, and prune_viewers
    // destroys windows and updates the tray icon, neither of which can be done
    // from a socket thread. Doing it there froze the application on the first
    // poll that raced the interface.
    prune_viewers();

    std::vector<WebWindow> out;

    auto describe = [](HWND hwnd, WebWindow& info) {
        if (!hwnd) return false;
        RECT rc{};
        if (!GetWindowRect(hwnd, &rc)) return false;

        WINDOWPLACEMENT placement{ sizeof(placement) };
        placement.length = sizeof(placement);
        GetWindowPlacement(hwnd, &placement);

        info.minimized = placement.showCmd == SW_SHOWMINIMIZED;
        info.maximized = placement.showCmd == SW_SHOWMAXIMIZED;

        // A minimised window's rectangle is parked far off screen, around
        // minus thirty two thousand. Reporting that puts it outside the panel
        // entirely, which looks exactly like it was closed. Its restored
        // position is what should be shown, dimmed.
        if (info.minimized) rc = placement.rcNormalPosition;

        info.x = rc.left;
        info.y = rc.top;
        info.width = rc.right - rc.left;
        info.height = rc.bottom - rc.top;
        return true;
    };

    for (const auto& viewer : viewers_) {
        if (!viewer || viewer->closed()) continue;
        WebWindow info;
        info.id      = viewer->id();
        info.kind    = "viewer";
        info.title   = util::narrow(viewer->title());
        info.address = viewer->address();
        if (describe(viewer->hwnd(), info)) out.push_back(std::move(info));
    }

    if (multiview_window_ && !multiview_window_->closed()) {
        WebWindow info;
        info.id    = 1;
        info.kind  = "multiview";
        info.title = "Multiview";
        if (describe(multiview_window_->hwnd(), info)) out.push_back(std::move(info));
    }

    {
        std::lock_guard<std::mutex> lock(web_mutex_);
        web_snapshot_.swap(out);
    }

    // The whole document is prepared here, once, however many browsers are
    // watching. The socket loop then only ever copies a string.
    if (web_server().running()) web_server().set_state(build_web_state());
}

void App::web_add_source(const std::string& typed) {
    Settings& cfg = settings();
    const std::string trimmed = util::trim(typed);
    if (trimmed.empty()) return;

    if (!omt::has_explicit_port(trimmed)) {
        // Same walk the source list does: a machine usually has more than one.
        std::string host, port;
        const std::string probe = omt::normalize_address(trimmed, cfg.port_start);
        if (omt::split_address(probe, &host, &port)) {
            scan_for_web_ = true;
            port_scanner().start(host, cfg.port_start, cfg.port_end, 10, hwnd_);
            return;
        }
    }

    const std::string normalized = omt::normalize_address(trimmed);
    if (normalized.empty()) return;
    const bool exists = std::any_of(cfg.manual_sources.begin(), cfg.manual_sources.end(),
                                    [&](const ManualSource& m) { return m.address == normalized; });
    if (exists) return;

    std::vector<std::string> taken;
    for (const auto& other : cfg.manual_sources) taken.push_back(other.name);

    ManualSource entry;
    entry.address = normalized;
    entry.name    = omt::safe_source_name("", omt::default_direct_name(taken));
    cfg.manual_sources.push_back(std::move(entry));
    cfg.save();
    discovery().set_manual_sources(cfg.manual_sources);
    util::logf("web: added source %s", normalized.c_str());
}

void App::apply_web_commands() {
    std::vector<WebCommand> commands;
    if (!web_server().take_commands(&commands)) return;

    prune_viewers();

    auto find_window = [this](int id) -> HWND {
        if (id == 1 && multiview_window_ && !multiview_window_->closed())
            return multiview_window_->hwnd();
        for (const auto& viewer : viewers_)
            if (viewer && !viewer->closed() && viewer->id() == id) return viewer->hwnd();
        return nullptr;
    };

    for (const auto& command : commands) {
        switch (command.kind) {
            case WebCommand::Kind::SetSource: {
                // A viewer is bound to its source for its lifetime, so changing
                // it means a new window. It is put back exactly where the old
                // one was, which is what makes it look like a change rather
                // than a replacement.
                HWND existing = find_window(command.id);
                RECT rc{};
                const bool had_rect = existing && GetWindowRect(existing, &rc);
                if (existing) {
                    SendMessageW(existing, WM_CLOSE, 0, 0);
                    prune_viewers();
                }
                open_viewer(command.address);
                if (had_rect && !viewers_.empty()) {
                    HWND fresh = viewers_.back()->hwnd();
                    if (fresh)
                        SetWindowPos(fresh, nullptr, rc.left, rc.top,
                                     rc.right - rc.left, rc.bottom - rc.top,
                                     SWP_NOZORDER | SWP_NOACTIVATE);
                }
                break;
            }
            case WebCommand::Kind::Move: {
                HWND hwnd = find_window(command.id);
                if (!hwnd) break;
                // Moving a maximised window has to restore it first or the
                // change is ignored.
                WINDOWPLACEMENT placement{ sizeof(placement) };
                GetWindowPlacement(hwnd, &placement);
                if (placement.showCmd == SW_SHOWMAXIMIZED) ShowWindow(hwnd, SW_RESTORE);
                // The same floor an interactive resize gets. SetWindowPos does
                // not consult WM_GETMINMAXINFO, so without this the panel could
                // take a window below any size the rest of the code expects.
                SetWindowPos(hwnd, nullptr, command.x, command.y,
                             std::max(kMinWindowWidth, command.width),
                             std::max(kMinWindowHeight, command.height),
                             SWP_NOZORDER | SWP_NOACTIVATE);
                break;
            }
            case WebCommand::Kind::WindowState: {
                HWND hwnd = find_window(command.id);
                if (!hwnd) break;
                if (command.state == "maximize")      ShowWindow(hwnd, SW_MAXIMIZE);
                else if (command.state == "minimize") ShowWindow(hwnd, SW_MINIMIZE);
                else                                  ShowWindow(hwnd, SW_RESTORE);
                break;
            }
            case WebCommand::Kind::OpenViewer:
                if (!command.address.empty()) open_viewer(command.address);
                break;
            case WebCommand::Kind::CloseWindow: {
                HWND hwnd = find_window(command.id);
                if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                break;
            }
            case WebCommand::Kind::MultiviewTile:
                multiview_engine().set_source(static_cast<size_t>(command.index),
                                              command.address);
                break;
            case WebCommand::Kind::MultiviewLayout:
                multiview_engine().set_layout(command.index);
                break;
            case WebCommand::Kind::OpenMultiview:
                show_multiview();
                break;
            case WebCommand::Kind::AddSource:
                web_add_source(command.address);
                break;
            default:
                break;
        }
    }

    refresh_web_snapshot();
}

LRESULT CALLBACK App::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App& app = App::instance();
    // Adopt the handle on creation, but never after quit() has released it:
    // the window is being torn down and must not be posted to again.
    if (!app.hwnd_ && !app.quitting_) app.hwnd_ = hwnd;
    if (!app.hwnd_) return DefWindowProcW(hwnd, msg, wp, lp);
    return app.handle(msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    static const UINT kTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (msg == kTaskbarCreated) {
        // Explorer restarted; the icon has to be re-added.
        tray_added_ = false;
        add_tray_icon();
        update_tray_tip();
        return 0;
    }

    switch (msg) {
        case WM_OMT_TRAY: {
            const UINT event = LOWORD(lp);
            if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
                show_sources();
            } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                show_tray_menu();
            }
            return 0;
        }

        case WM_OMT_SOURCES:
            update_tray_tip();
            if (sources_window_) sources_window_->invalidate();
            if (settings_window_) settings_window_->invalidate();
            return 0;

        case WM_OMT_SHOWMAIN:
            show_sources();
            return 0;

        case WM_OMT_INSTALL:
            do_install_update();
            return 0;

        case WM_OMT_MVOUTPUT:
            do_toggle_multiview_output();
            return 0;

        case WM_OMT_WEBCMD:
            apply_web_commands();
            return 0;

        case WM_OMT_SCAN:
            // A walk started from the control panel has nobody watching it, so
            // its results are taken here.
            if (wp == 1 && scan_for_web_) {
                scan_for_web_ = false;
                Settings& cfg = settings();
                int added = 0;
                for (const auto& hit : port_scanner().state().hits) {
                    const bool exists = std::any_of(
                        cfg.manual_sources.begin(), cfg.manual_sources.end(),
                        [&](const ManualSource& m) { return m.address == hit.address; });
                    if (exists) continue;
                    std::vector<std::string> taken;
                    for (const auto& other : cfg.manual_sources) taken.push_back(other.name);
                    ManualSource entry;
                    entry.address = hit.address;
                    entry.name = omt::safe_source_name(hit.product,
                                                       omt::default_direct_name(taken));
                    cfg.manual_sources.push_back(std::move(entry));
                    ++added;
                }
                if (added > 0) {
                    cfg.save();
                    discovery().set_manual_sources(cfg.manual_sources);
                    util::logf("web: walk added %d source(s)", added);
                }
            }
            return 0;

        case WM_OMT_UPDATE: {
            if (sources_window_) sources_window_->invalidate();
            if (settings_window_) settings_window_->invalidate();
            const UpdateInfo up = updater().info();
            // Tell the user once that something is available. Downloading and
            // installing still wait for a press.
            if (up.state == UpdateState::Available && !update_announced_) {
                update_announced_ = true;
                notify(L"OMT Mini update available",
                       util::widen("Version " + up.latest_version +
                                   " is ready to download. Open Settings to install it."));
            }
            return 0;
        }

        case WM_COMMAND: {
            const UINT id = LOWORD(wp);
            if (id >= kIdSourcesFirst && id <= kIdSourcesLast) {
                const size_t index = id - kIdSourcesFirst;
                if (index < menu_sources_.size()) open_viewer(menu_sources_[index]);
                return 0;
            }
            switch (id) {
                case kIdShowSources:    show_sources(); return 0;
                case kIdMultiview:      show_multiview(); return 0;
                case kIdMultiviewOut:   toggle_multiview_output(); return 0;
                case kIdSettings:       show_settings(settings_tab::General); return 0;
                case kIdCloseViewers:   close_all_viewers(); return 0;
                case kIdDesktopCapture: toggle_desktop_capture(); return 0;
                case kIdWebcam:         toggle_webcam(); return 0;
                case kIdUpdate:         show_settings(settings_tab::About); return 0;
                case kIdOpenLog:
                    ShellExecuteW(nullptr, L"open", util::config_dir().c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdExit:           quit(); return 0;
                default: break;
            }
            return 0;
        }

        case WM_TIMER:
            if (web_server().running()) refresh_web_snapshot();
            else                        prune_viewers();
            return 0;

        case WM_CLOSE:
            // The installer sends this to a running copy before replacing its
            // files, so it has to be a full shutdown rather than a hide.
            quit();
            return 0;

        case WM_ENDSESSION:
            if (wp) quit();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

bool App::init(HINSTANCE instance) {
    instance_ = instance;
    icon_ = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);

    if (!create_message_window(instance)) {
        util::logf("app: message window creation failed");
        return false;
    }
    if (!add_tray_icon()) {
        MessageBoxW(dialog_owner(),
                    L"OMT Mini could not create its tray icon.",
                    L"OMT Mini", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        return false;
    }

    announcer().start();
    discovery().set_manual_sources(settings().manual_sources);
    discovery().start(hwnd_, WM_OMT_SOURCES);
    // Twice a second: the control panel wants window positions kept current,
    // and pruning closed viewers at this rate costs nothing.
    SetTimer(hwnd_, 99, 500, nullptr);

    const Settings& cfg = settings();

    if (cfg.web_enabled) {
        if (web_server().start(cfg.web_port, hwnd_)) refresh_web_snapshot();
        else util::logf("web: could not start: %s", web_server().error().c_str());
    }
    if (cfg.capture_autostart) desktop_capture().start();
    if (cfg.webcam_autostart && !cfg.webcam_source.empty() &&
        WebcamOutput::filter_registered())
        webcam().start(cfg.webcam_source);

    // The output runs whether or not the window is open: a machine can build a
    // wall and hand it to everyone else without showing it locally.
    if (cfg.multiview_output_enabled) multiview_output().start();
    if (cfg.multiview_autostart) show_multiview();

    if (settings().check_updates_on_launch) {
        // Quiet: a machine with no route to GitHub should not open a dialog
        // about it every time it starts.
        updater().check(hwnd_, true);
    }

    update_tray_tip();
    return true;
}

void App::quit() {
    if (quitting_) return;
    quitting_ = true;

    util::logf("app: shutting down");
    close_all_viewers();
    viewers_.clear();
    multiview_window_.reset();
    sources_window_.reset();
    settings_window_.reset();

    web_server().stop();
    announcer().stop();
    multiview_output().stop();
    webcam().stop();
    desktop_capture().stop();
    discovery().stop();
    updater().cancel();
    updater().join();
    remove_tray_icon();

    if (hwnd_) {
        KillTimer(hwnd_, 99);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    PostQuitMessage(0);
}

int App::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
