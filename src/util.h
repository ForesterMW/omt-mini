// OMT Mini - small shared helpers.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace util {

// ---- string conversion -------------------------------------------------
std::wstring widen(const char* s, int len = -1);
std::wstring widen(const std::string& s);
std::string narrow(const wchar_t* s, int len = -1);
std::string narrow(const std::wstring& s);

// ---- paths -------------------------------------------------------------
// %APPDATA%\OMT Mini, created on first use. Settings and logs live here.
std::wstring config_dir();
std::wstring config_path(const wchar_t* leaf);
// Directory containing the running executable (where libomt.dll is expected).
std::wstring exe_dir();

// ---- logging -----------------------------------------------------------
// Appends to %APPDATA%\OMT Mini\omtmini.log. Safe from any thread.
void log_init();
void log_shutdown();
void logf(const char* fmt, ...);
std::wstring log_path();

// ---- time --------------------------------------------------------------
// Monotonic milliseconds since process start.
int64_t now_ms();
// OMT timestamps run at 10,000,000 ticks per second.
int64_t now_omt_ticks();

// ---- misc --------------------------------------------------------------
std::string trim(const std::string& s);
bool iequals(const std::string& a, const std::string& b);

// RAII for COM interface pointers. Deliberately minimal: we only need
// construction, reset, release and the arrow operator.
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }

    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T*  get() const { return p_; }
    T*  operator->() const { return p_; }
    T** put() { reset(); return &p_; }              // for out-params
    void** put_void() { reset(); return reinterpret_cast<void**>(&p_); }
    explicit operator bool() const { return p_ != nullptr; }
    T* detach() { T* t = p_; p_ = nullptr; return t; }

private:
    T* p_ = nullptr;
};

} // namespace util
