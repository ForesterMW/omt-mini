#include "app.h"
#include "sourceswin.h"
#include "settingswin.h"
#include "discovery.h"
#include "capture.h"
#include "webcam.h"
#include "settings.h"
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

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdSettings, L"Settings...");
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
        MessageBoxW(nullptr, L"The viewer window could not be created.",
                    L"OMT Mini", MB_OK | MB_ICONERROR);
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
        MessageBoxW(nullptr,
                    L"Desktop capture could not start. See the log for details.",
                    L"OMT Mini", MB_OK | MB_ICONWARNING);
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
        MessageBoxW(nullptr,
                    L"Choose a source for the webcam output in Settings first.",
                    L"OMT Mini", MB_OK | MB_ICONINFORMATION);
        show_settings(4);
        return false;
    }
    if (!WebcamOutput::filter_registered()) {
        const int answer = MessageBoxW(nullptr,
            L"The OMT Mini virtual camera is not registered yet.\n\n"
            L"Register it now for this user? No administrator rights are needed.",
            L"OMT Mini", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) return false;

        std::wstring message;
        if (!WebcamOutput::register_filter(true, &message)) {
            MessageBoxW(nullptr, message.c_str(), L"OMT Mini", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    return webcam().start(source);
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

        case WM_COMMAND: {
            const UINT id = LOWORD(wp);
            if (id >= kIdSourcesFirst && id <= kIdSourcesLast) {
                const size_t index = id - kIdSourcesFirst;
                if (index < menu_sources_.size()) open_viewer(menu_sources_[index]);
                return 0;
            }
            switch (id) {
                case kIdShowSources:    show_sources(); return 0;
                case kIdSettings:       show_settings(0); return 0;
                case kIdCloseViewers:   close_all_viewers(); return 0;
                case kIdDesktopCapture: toggle_desktop_capture(); return 0;
                case kIdWebcam:         toggle_webcam(); return 0;
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
            prune_viewers();
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
        MessageBoxW(nullptr,
                    L"OMT Mini could not create its tray icon.",
                    L"OMT Mini", MB_OK | MB_ICONERROR);
        return false;
    }

    discovery().start(hwnd_, WM_OMT_SOURCES);
    SetTimer(hwnd_, 99, 1000, nullptr);

    const Settings& cfg = settings();
    if (cfg.capture_autostart) desktop_capture().start();
    if (cfg.webcam_autostart && !cfg.webcam_source.empty() &&
        WebcamOutput::filter_registered())
        webcam().start(cfg.webcam_source);

    update_tray_tip();
    return true;
}

void App::quit() {
    if (quitting_) return;
    quitting_ = true;

    util::logf("app: shutting down");
    close_all_viewers();
    viewers_.clear();
    sources_window_.reset();
    settings_window_.reset();

    webcam().stop();
    desktop_capture().stop();
    discovery().stop();
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
