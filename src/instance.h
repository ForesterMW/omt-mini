// Finding and shutting down a running copy of OMT Mini.
//
// Shared by the application, the uninstaller and the installer, which is a
// separate binary, so everything here is inline and depends on nothing but
// windows.h.
#pragma once
#include <windows.h>

#define OMTMINI_TRAY_CLASS     L"OMTMiniTray"
#define OMTMINI_INSTANCE_MUTEX L"Local\\OMTMini.SingleInstance"

namespace instance {

// The tray window is created as a child of HWND_MESSAGE, which makes it a
// message-only window. FindWindowW only walks top level windows and will never
// return it, so the parent has to be named explicitly.
inline HWND find_tray_window() {
    HWND window = FindWindowExW(HWND_MESSAGE, nullptr, OMTMINI_TRAY_CLASS, nullptr);
    if (!window) window = FindWindowW(OMTMINI_TRAY_CLASS, nullptr);
    return window;
}

// The authoritative liveness test. A running copy holds this mutex for its
// whole lifetime, and the kernel releases it when the process ends however it
// ends, including a crash. A window lookup can miss; this cannot.
inline bool running() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, OMTMINI_INSTANCE_MUTEX);
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}

// Asks a running copy to quit, then waits for the process to actually go.
// `pump` lets a caller with a window keep painting while it waits.
// Returns true when nothing is running any more.
inline bool request_quit_and_wait(int timeout_ms, void (*pump)() = nullptr) {
    if (!running()) return true;

    if (HWND window = find_tray_window())
        PostMessageW(window, WM_CLOSE, 0, 0);

    constexpr int kStepMs = 100;
    for (int waited = 0; waited < timeout_ms; waited += kStepMs) {
        Sleep(kStepMs);
        if (pump) pump();
        if (!running()) {
            // The mutex goes as the process exits; give the file handles a
            // moment to be released behind it.
            Sleep(250);
            return true;
        }
    }
    return !running();
}

} // namespace instance
