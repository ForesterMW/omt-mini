// OMT Mini entry point.
//
// One process, one tray icon. Everything else (viewers, desktop capture, the
// webcam output) hangs off the App object.
#include "app.h"
#include "settings.h"
#include "discovery.h"
#include "omt.h"
#include "gfx.h"
#include "util.h"
#include "uninstall.h"
#include "instance.h"

#include <objbase.h>

namespace {

constexpr wchar_t kInstanceMutex[] = OMTMINI_INSTANCE_MUTEX;

// Per monitor v2 keeps text crisp when a viewer is dragged between displays.
void enable_dpi_awareness() {
    using PFN_SetCtx = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto set_ctx = reinterpret_cast<PFN_SetCtx>(reinterpret_cast<void*>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        // -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
        if (set_ctx && set_ctx(reinterpret_cast<HANDLE>(-4))) return;
    }
    using PFN_SetDpiAware = BOOL(WINAPI*)();
    if (user32) {
        auto legacy = reinterpret_cast<PFN_SetDpiAware>(reinterpret_cast<void*>(
            GetProcAddress(user32, "SetProcessDPIAware")));
        if (legacy) legacy();
    }
}

// Returns true when another copy is already running, after asking it to show.
bool hand_off_to_existing_instance(HANDLE* mutex_out) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Message-only window, so it has to be looked up under HWND_MESSAGE.
        if (HWND existing = instance::find_tray_window())
            PostMessageW(existing, WM_OMT_SHOWMAIN, 0, 0);
        CloseHandle(mutex);
        return true;
    }
    *mutex_out = mutex;
    return false;
}

bool has_arg(const wchar_t* needle) {
    const wchar_t* cmd = GetCommandLineW();
    return cmd && wcsstr(cmd, needle) != nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    enable_dpi_awareness();

    // Uninstall runs before the single instance guard so it can shut down a
    // copy that is already in the tray.
    if (uninstall::requested()) {
        uninstall::run();
        return 0;
    }

    HANDLE instance_mutex = nullptr;
    if (hand_off_to_existing_instance(&instance_mutex)) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    util::log_init();
    util::logf("=== OMT Mini %s starting ===", OMTMINI_VERSION);

    settings().load();

    // libomt is the one hard dependency; without it there is nothing to show.
    if (!omt::load()) {
        const std::wstring message =
            L"OMT Mini could not start.\n\n" + omt::load_error() +
            L"\n\nlibomt.dll and libvmx.dll ship alongside OMTMini.exe. If you moved "
            L"the executable, move them with it.";
        MessageBoxW(nullptr, message.c_str(), L"OMT Mini", MB_OK | MB_ICONERROR);
        util::logf("startup: libomt unavailable, exiting");
        util::log_shutdown();
        CoUninitialize();
        if (instance_mutex) CloseHandle(instance_mutex);
        return 1;
    }

    settings().apply_to_libomt();

    if (!gfx::Device::init()) {
        const std::wstring message =
            L"OMT Mini could not initialise Direct3D 11.\n\n" + gfx::Device::error();
        MessageBoxW(nullptr, message.c_str(), L"OMT Mini", MB_OK | MB_ICONERROR);
        util::logf("startup: gfx init failed, exiting");
        omt::shutdown();
        util::log_shutdown();
        CoUninitialize();
        if (instance_mutex) CloseHandle(instance_mutex);
        return 1;
    }

    int exit_code = 1;
    App& app = App::instance();
    if (app.init(instance)) {
        // --tray is used by the start-with-Windows entry, and suppresses the
        // window even when the user normally wants it on launch.
        if (settings().show_sources_on_start && !has_arg(L"--tray"))
            app.show_sources();

        exit_code = app.run();
    }

    util::logf("=== OMT Mini exiting ===");
    gfx::Device::teardown();
    omt::shutdown();
    util::log_shutdown();
    CoUninitialize();
    if (instance_mutex) CloseHandle(instance_mutex);
    return exit_code;
}
