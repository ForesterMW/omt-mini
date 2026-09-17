#include "window.h"
#include "util.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>

namespace {
constexpr UINT kDwmUseImmersiveDarkMode = 20;
constexpr UINT kDwmWindowCornerPreference = 33;
constexpr UINT kDwmCornerRound = 2;
constexpr UINT kDwmCornerDoNotRound = 1;

// GetDpiForWindow is Windows 10 1607 and later; fall back to the desktop DPI.
UINT window_dpi(HWND hwnd) {
    using PFN = UINT(WINAPI*)(HWND);
    static PFN fn = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<PFN>(reinterpret_cast<void*>(
                            GetProcAddress(user32, "GetDpiForWindow")))
                      : nullptr;
    }();
    if (fn) {
        const UINT dpi = fn(hwnd);
        if (dpi >= 72 && dpi <= 480) return dpi;
    }
    HDC dc = GetDC(nullptr);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}
} // namespace

void apply_window_theme(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    // Older Windows 10 builds used attribute 19 for the same thing.
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    set_window_rounded(hwnd, true);
}

void set_window_rounded(HWND hwnd, bool rounded) {
    const DWORD corner = rounded ? kDwmCornerRound : kDwmCornerDoNotRound;
    DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
}

Window::~Window() { destroy(); }

bool Window::create(const std::wstring& title, int width, int height,
                    bool resizable, HWND owner) {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = &Window::wnd_proc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;      // fully painted by Direct2D
    wc.lpszClassName = class_name();
    wc.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);           // harmless if already registered

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

    // Size the client area, not the outer frame.
    RECT rc{ 0, 0, width, height };
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    hwnd_ = CreateWindowExW(0, class_name(), title.c_str(), style,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            owner, nullptr, instance, this);
    if (!hwnd_) {
        util::logf("window: CreateWindowEx failed err=%lu", GetLastError());
        return false;
    }

    dpi_ = window_dpi(hwnd_);
    apply_window_theme(hwnd_);

    // Re-apply the client size now that the DPI is known.
    if (dpi_ != 96) {
        const int sw = MulDiv(width, dpi_, 96);
        const int sh = MulDiv(height, dpi_, 96);
        RECT sc{ 0, 0, sw, sh };
        AdjustWindowRectEx(&sc, style, FALSE, 0);
        SetWindowPos(hwnd_, nullptr, 0, 0, sc.right - sc.left, sc.bottom - sc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!surface_.create(hwnd_)) {
        util::logf("window: surface creation failed");
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void Window::destroy() {
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        surface_.destroy();
        DestroyWindow(h);
    }
}

void Window::show(bool activate) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) {
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
    }
    invalidate();
}

void Window::hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }

bool Window::visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void Window::invalidate() {
    if (!hwnd_) return;
    // No local "already pending" guard: Windows coalesces overlapping update
    // regions into a single WM_PAINT by itself, and a guard that latched on a
    // paint that never arrived would silently freeze the window.
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::set_always_on_top(bool on) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Window::center_on_cursor() {
    if (!hwnd_) return;
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;

    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::render_frame() {
    if (!hwnd_ || !surface_.valid() || in_render_) return;
    in_render_ = true;

    // One frame is a sequence of target binds and draws, so it has to be
    // atomic against anything else using the shared device, such as the
    // multiview output composing on its own thread.
    gfx::DeviceLock device_lock;

    if (surface_.begin_frame()) {
        surface_.d3d_target();
        surface_.clear(clear_colour());
        on_render_video();

        ID2D1DeviceContext* dc = surface_.d2d_begin();
        if (dc) {
            // Working in DIPs keeps the layout identical at any scaling.
            dc->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
            const float scale = 96.0f / static_cast<float>(dpi_);
            ctx_.begin(dc, surface_.width() * scale, surface_.height() * scale, &input_);
            on_render(ctx_);
            ctx_.end();
        }
        surface_.present(true);
    }

    input_.clear_events();
    in_render_ = false;

    // A widget mid animation asks for one more frame.
    if (ctx_.needs_redraw()) invalidate();
}

void Window::update_mouse(LPARAM lp) {
    const float scale = 96.0f / static_cast<float>(dpi_);
    input_.mouse_x = static_cast<float>(GET_X_LPARAM(lp)) * scale;
    input_.mouse_y = static_cast<float>(GET_Y_LPARAM(lp)) * scale;

    if (!tracking_mouse_) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        tracking_mouse_ = true;
    }
}

LRESULT CALLBACK Window::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT Window::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result = 0;
    if (on_message(msg, wp, lp, result)) return result;

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            render_frame();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE: {
            const UINT w = LOWORD(lp), h = HIWORD(lp);
            if (wp != SIZE_MINIMIZED && w > 0 && h > 0) {
                surface_.resize(w, h);
                on_resize(w, h);
                render_frame();
            }
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            auto* rc = reinterpret_cast<RECT*>(lp);
            if (rc)
                SetWindowPos(hwnd_, nullptr, rc->left, rc->top,
                             rc->right - rc->left, rc->bottom - rc->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            invalidate();
            return 0;
        }

        case WM_MOUSEMOVE:
            update_mouse(lp);
            invalidate();
            return 0;

        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            input_.mouse_x = input_.mouse_y = -1.0f;
            invalidate();
            return 0;

        case WM_LBUTTONDOWN:
            SetCapture(hwnd_);
            update_mouse(lp);
            input_.mouse_down = true;
            input_.mouse_pressed = true;
            invalidate();
            return 0;

        case WM_LBUTTONDBLCLK:
            update_mouse(lp);
            input_.mouse_double = true;
            input_.mouse_down = true;
            input_.mouse_pressed = true;
            invalidate();
            return 0;

        case WM_LBUTTONUP:
            ReleaseCapture();
            update_mouse(lp);
            input_.mouse_down = false;
            input_.mouse_released = true;
            invalidate();
            return 0;

        case WM_RBUTTONUP:
            update_mouse(lp);
            input_.right_pressed = true;
            invalidate();
            return 0;

        case WM_MOUSEWHEEL: {
            // Wheel coordinates are screen relative, unlike the other messages.
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd_, &pt);
            const float scale = 96.0f / static_cast<float>(dpi_);
            input_.mouse_x = pt.x * scale;
            input_.mouse_y = pt.y * scale;
            input_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            invalidate();
            return 0;
        }

        case WM_CHAR:
            if (wp >= 8 && wp != 27) {
                input_.typed.push_back(static_cast<wchar_t>(wp));
                invalidate();
            }
            return 0;

        case WM_KEYDOWN:
            input_.key = static_cast<int>(wp);
            input_.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            input_.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            invalidate();
            return 0;

        case WM_OMT_REDRAW:
        case WM_OMT_FRAME:
            invalidate();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = MulDiv(kMinWindowWidth, dpi_, 96);
            mmi->ptMinTrackSize.y = MulDiv(kMinWindowHeight, dpi_, 96);
            return 0;
        }

        case WM_CLOSE:
            if (!on_close_request()) return 0;
            on_closing();
            return 0;

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
