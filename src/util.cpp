#include "util.h"

#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <algorithm>
#include <cctype>

namespace util {
namespace {
std::mutex     g_log_mutex;
HANDLE         g_log_file = INVALID_HANDLE_VALUE;
LARGE_INTEGER  g_qpc_freq{};
LARGE_INTEGER  g_qpc_start{};
bool           g_qpc_ready = false;

void ensure_qpc() {
    if (g_qpc_ready) return;
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_start);
    g_qpc_ready = true;
}
} // namespace

std::wstring widen(const char* s, int len) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, out.data(), n);
    if (len == -1 && !out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}
std::wstring widen(const std::string& s) {
    return widen(s.c_str(), static_cast<int>(s.size()));
}

std::string narrow(const wchar_t* s, int len) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    if (len == -1 && !out.empty() && out.back() == '\0') out.pop_back();
    return out;
}
std::string narrow(const std::wstring& s) {
    return narrow(s.c_str(), static_cast<int>(s.size()));
}

std::wstring config_dir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    PWSTR roaming = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        dir = roaming;
        CoTaskMemFree(roaming);
    } else {
        wchar_t buf[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, buf);
        dir = buf;
        if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    }
    dir += L"\\OMT Mini";
    CreateDirectoryW(dir.c_str(), nullptr);
    cached = dir;
    return cached;
}

std::wstring config_path(const wchar_t* leaf) {
    return config_dir() + L"\\" + leaf;
}

std::wstring exe_dir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) break;
        buf.resize(buf.size() * 2);
    }
    std::wstring path(buf.data());
    size_t slash = path.find_last_of(L'\\');
    cached = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    return cached;
}

std::wstring log_path() { return config_path(L"omtmini.log"); }

void log_init() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file != INVALID_HANDLE_VALUE) return;

    const std::wstring path = log_path();

    // Roll the log once it passes 1 MB so it cannot grow without bound on a
    // machine that is left running for weeks.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER size;
        size.LowPart  = fad.nFileSizeLow;
        size.HighPart = fad.nFileSizeHigh;
        if (size.QuadPart > 1024 * 1024) {
            const std::wstring old = path + L".1";
            DeleteFileW(old.c_str());
            MoveFileW(path.c_str(), old.c_str());
        }
    }

    g_log_file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log_shutdown() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
}

void logf(const char* fmt, ...) {
    char body[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[2176];
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%lu] %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentThreadId(), body);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line));

    OutputDebugStringA(line);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_log_file, line, static_cast<DWORD>(n), &written, nullptr);
}

int64_t now_ms() {
    ensure_qpc();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return ((now.QuadPart - g_qpc_start.QuadPart) * 1000) / g_qpc_freq.QuadPart;
}

int64_t now_omt_ticks() {
    ensure_qpc();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Scale to 10 MHz without overflowing: split the division.
    const int64_t ticks = now.QuadPart - g_qpc_start.QuadPart;
    const int64_t secs  = ticks / g_qpc_freq.QuadPart;
    const int64_t rem   = ticks % g_qpc_freq.QuadPart;
    return secs * 10000000LL + (rem * 10000000LL) / g_qpc_freq.QuadPart;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

} // namespace util
