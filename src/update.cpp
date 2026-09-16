#include "update.h"
#include "window.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

Updater g_updater;

const wchar_t* kApiHost   = L"api.github.com";
const wchar_t* kApiPath   = L"/repos/ForesterMW/omt-mini/releases/latest";
const wchar_t* kUserAgent = L"OMTMini-Updater/" OMTMINI_VERSION_W;

// ---- tiny JSON field extraction ----------------------------------------
// The shape of a GitHub release is stable and small, so a targeted scanner is
// preferable to taking on a JSON dependency for four fields.
std::string json_unescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        switch (in[++i]) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case 'r':  out += '\r'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case '/':  out += '/';  break;
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case 'u': {
                if (i + 4 >= in.size()) break;
                const std::string hex = in.substr(i + 1, 4);
                i += 4;
                const auto cp = static_cast<wchar_t>(std::strtol(hex.c_str(), nullptr, 16));
                out += util::narrow(std::wstring(1, cp));
                break;
            }
            default: out += in[i]; break;
        }
    }
    return out;
}

// Value of "key": "..." starting the search at `from`.
std::string json_string(const std::string& json, const std::string& key, size_t from = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
                                 json[pos] == '\r' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    std::string raw;
    while (pos < json.size()) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            raw += json[pos];
            raw += json[pos + 1];
            pos += 2;
            continue;
        }
        if (json[pos] == '"') break;
        raw += json[pos++];
    }
    return json_unescape(raw);
}

int64_t json_number(const std::string& json, const std::string& key, size_t from = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0;
    ++pos;
    while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    return _strtoi64(json.c_str() + pos, nullptr, 10);
}

// ---- WinHTTP ------------------------------------------------------------
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

HINTERNET open_session() {
    // Automatic proxy discovery matters on a corporate network; fall back to
    // the machine's configured proxy on builds that do not support it.
    HINTERNET s = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        s = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (s) {
        DWORD timeout = 20000;
        WinHttpSetTimeouts(s, timeout, timeout, timeout, timeout);
    }
    return s;
}

bool split_url(const std::string& url, std::wstring* host, std::wstring* path, bool* https) {
    const std::wstring wide = util::widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize     = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength  = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength= static_cast<DWORD>(-1);
    uc.dwSchemeLength   = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &uc)) return false;

    *host  = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
    *path  = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) *path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    *https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

// Performs a GET. If `to_file` is set, the body is streamed there and
// `progress` is updated; otherwise the body is returned in `out`.
bool http_get(const std::wstring& host, const std::wstring& path, bool https,
              std::string* out, HANDLE to_file,
              const std::function<void(int64_t, int64_t)>& progress,
              std::string* error, const std::atomic<bool>* cancel = nullptr) {
    Handle session{ open_session() };
    if (!session) { if (error) *error = "Could not start a network session."; return false; }

    Handle conn{ WinHttpConnect(session, host.c_str(),
                                https ? INTERNET_DEFAULT_HTTPS_PORT
                                      : INTERNET_DEFAULT_HTTP_PORT, 0) };
    if (!conn) { if (error) *error = "Could not reach " + util::narrow(host) + "."; return false; }

    Handle req{ WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   https ? WINHTTP_FLAG_SECURE : 0) };
    if (!req) { if (error) *error = "Could not build the request."; return false; }

    WinHttpAddRequestHeaders(req,
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        if (error) *error = "No response from " + util::narrow(host) +
                            ". Check the network connection.";
        return false;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        char buf[96];
        snprintf(buf, sizeof(buf), "GitHub returned HTTP %lu.", status);
        if (error) *error = buf;
        return false;
    }

    int64_t total = 0;
    DWORD len_size = sizeof(total);
    DWORD len64 = 0;
    len_size = sizeof(len64);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &len64, &len_size,
                            WINHTTP_NO_HEADER_INDEX))
        total = len64;

    std::vector<char> buffer(64 * 1024);
    int64_t received = 0;
    for (;;) {
        if (cancel && cancel->load()) {
            if (error) *error = "The download was cancelled.";
            return false;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) {
            if (error) *error = "The transfer was interrupted.";
            return false;
        }
        if (available == 0) break;

        while (available > 0) {
            if (cancel && cancel->load()) {
                if (error) *error = "The download was cancelled.";
                return false;
            }
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            DWORD got = 0;
            if (!WinHttpReadData(req, buffer.data(), want, &got) || got == 0) {
                if (error) *error = "The transfer was interrupted.";
                return false;
            }
            if (to_file) {
                DWORD written = 0;
                if (!WriteFile(to_file, buffer.data(), got, &written, nullptr) ||
                    written != got) {
                    if (error) *error = "Could not write the downloaded file.";
                    return false;
                }
            } else if (out) {
                out->append(buffer.data(), got);
            }
            received += got;
            available -= got;
            if (progress) progress(received, total);
        }
    }
    return true;
}

bool http_get_url(const std::string& url, std::string* out, HANDLE to_file,
                  const std::function<void(int64_t, int64_t)>& progress,
                  std::string* error, const std::atomic<bool>* cancel = nullptr) {
    std::wstring host, path;
    bool https = true;
    if (!split_url(url, &host, &path, &https)) {
        if (error) *error = "That download address could not be understood.";
        return false;
    }
    return http_get(host, path, https, out, to_file, progress, error, cancel);
}

// ---- SHA-256 ------------------------------------------------------------
std::string sha256_file(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    std::vector<UCHAR> hash_object, digest(32);
    DWORD object_size = 0, copied = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &copied, 0) != 0) goto done;
    hash_object.resize(object_size);
    if (BCryptCreateHash(alg, &hash, hash_object.data(), object_size, nullptr, 0, 0) != 0)
        goto done;

    {
        std::vector<UCHAR> buffer(256 * 1024);
        DWORD got = 0;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) &&
               got > 0) {
            if (BCryptHashData(hash, buffer.data(), got, 0) != 0) goto done;
        }
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0)
            goto done;

        char hex[65];
        for (size_t i = 0; i < digest.size(); ++i)
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        result.assign(hex, 64);
    }

done:
    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return result;
}

std::wstring temp_download_path(const std::string& name) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + util::widen(name);
}

} // namespace

Updater& updater() { return g_updater; }

int Updater::compare_versions(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s) {
        int parts[3] = { 0, 0, 0 };
        size_t pos = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
        for (int i = 0; i < 3 && pos < s.size(); ++i) {
            parts[i] = std::atoi(s.c_str() + pos);
            const size_t dot = s.find('.', pos);
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
        return std::array<int, 3>{ parts[0], parts[1], parts[2] };
    };
    const auto va = parse(a), vb = parse(b);
    for (int i = 0; i < 3; ++i) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return 1;
    }
    return 0;
}

UpdateInfo Updater::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

bool Updater::busy() const { return busy_; }

bool Updater::update_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_.state == UpdateState::Available ||
           info_.state == UpdateState::Downloading ||
           info_.state == UpdateState::ReadyToInstall;
}

void Updater::set(const UpdateInfo& next) {
    std::lock_guard<std::mutex> lock(mutex_);
    info_ = next;
}

void Updater::cancel() { cancel_ = true; }

void Updater::join() {
    if (thread_.joinable()) thread_.join();
}

void Updater::check(HWND notify, bool quiet) {
    if (busy_.exchange(true)) return;
    join();
    cancel_ = false;
    thread_ = std::thread([this, notify, quiet] {
        run_check(notify, quiet);
        busy_ = false;
    });
}

void Updater::download_and_install(HWND notify) {
    if (busy_.exchange(true)) return;
    join();
    cancel_ = false;
    thread_ = std::thread([this, notify] {
        run_install(notify);
        busy_ = false;
    });
}

void Updater::run_check(HWND notify, bool quiet) {
    UpdateInfo next;
    next.current_version = OMTMINI_VERSION;
    next.state = UpdateState::Checking;
    set(next);
    if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);

    std::string body, error;
    if (!http_get(kApiHost, kApiPath, true, &body, nullptr, nullptr, &error)) {
        next.state = UpdateState::Failed;
        next.error = quiet ? std::string() : error;
        util::logf("update: check failed: %s", error.c_str());
        set(next);
        if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);
        return;
    }

    // /releases/latest already excludes drafts and prereleases, so whatever it
    // returns is by definition the latest stable release.
    const std::string tag = json_string(body, "tag_name");
    next.latest_version = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
                        ? tag.substr(1) : tag;
    next.release_url = json_string(body, "html_url");

    // Walk the asset array for the installer and the checksum list.
    size_t pos = body.find("\"assets\"");
    while (pos != std::string::npos) {
        const size_t name_at = body.find("\"name\"", pos);
        if (name_at == std::string::npos) break;
        const std::string name = json_string(body, "name", pos);
        const std::string url  = json_string(body, "browser_download_url", name_at);
        if (url.empty()) break;

        if (name.rfind("OMTMini-Setup-", 0) == 0 && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".exe") == 0) {
            next.setup_url  = url;
            next.setup_name = name;
            next.setup_size = json_number(body, "size", name_at);
        } else if (name.rfind("SHA256SUMS", 0) == 0) {
            next.checksums_url = url;
        }
        // Step past this asset's download url so the next pass lands on the
        // following asset's name rather than re-reading this one.
        const size_t url_at = body.find("\"browser_download_url\"", name_at);
        pos = (url_at == std::string::npos) ? std::string::npos
                                            : url_at + sizeof("\"browser_download_url\"");
    }

    if (next.latest_version.empty()) {
        next.state = UpdateState::Failed;
        next.error = quiet ? std::string() : "The release information could not be read.";
    } else if (compare_versions(next.current_version, next.latest_version) < 0 &&
               !next.setup_url.empty()) {
        next.state = UpdateState::Available;
        util::logf("update: %s available (running %s)", next.latest_version.c_str(),
                   next.current_version.c_str());
    } else {
        next.state = UpdateState::UpToDate;
        util::logf("update: up to date at %s", next.current_version.c_str());
    }

    set(next);
    if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);
}

void Updater::run_install(HWND notify) {
    UpdateInfo next = info();
    if (next.setup_url.empty()) {
        next.state = UpdateState::Failed;
        next.error = "There is no installer to download.";
        set(next);
        if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);
        return;
    }

    next.state = UpdateState::Downloading;
    next.progress_percent = 0;
    set(next);
    if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);

    const std::wstring target = temp_download_path(next.setup_name);
    DeleteFileW(target.c_str());

    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        next.state = UpdateState::Failed;
        next.error = "Could not create the download file.";
        set(next);
        if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);
        return;
    }

    int last_percent = -1;
    std::string error;
    const bool ok = http_get_url(next.setup_url, nullptr, file,
        [&](int64_t received, int64_t total) {
            const int percent = total > 0
                ? static_cast<int>((received * 100) / total)
                : 0;
            if (percent != last_percent) {
                last_percent = percent;
                UpdateInfo p = info();
                p.progress_percent = percent;
                set(p);
                if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);
            }
        }, &error, &cancel_);
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(target.c_str());
        next = info();
        next.state = UpdateState::Failed;
        next.error = error;
        util::logf("update: download failed: %s", error.c_str());
        set(next);
        if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);
        return;
    }

    // Verify against the checksum file published alongside the installer. A
    // truncated or tampered download must never be executed.
    if (!next.checksums_url.empty()) {
        std::string sums;
        if (http_get_url(next.checksums_url, &sums, nullptr, nullptr, &error)) {
            const std::string actual = sha256_file(target);
            bool matched = false;
            size_t line_start = 0;
            while (line_start < sums.size()) {
                size_t eol = sums.find('\n', line_start);
                if (eol == std::string::npos) eol = sums.size();
                const std::string line = sums.substr(line_start, eol - line_start);
                line_start = eol + 1;
                if (line.find(next.setup_name) == std::string::npos) continue;
                if (line.size() >= 64 && util::iequals(line.substr(0, 64), actual))
                    matched = true;
                break;
            }
            if (!matched) {
                DeleteFileW(target.c_str());
                next = info();
                next.state = UpdateState::Failed;
                next.error = "The download did not match its published checksum "
                             "and has been discarded.";
                util::logf("update: checksum mismatch, discarded download");
                set(next);
                if (notify) PostMessageW(notify, WM_OMT_UPDATE, 0, 0);
                return;
            }
            util::logf("update: checksum verified");
        } else {
            util::logf("update: could not fetch checksums: %s", error.c_str());
        }
    }

    next = info();
    next.state = UpdateState::ReadyToInstall;
    next.downloaded_path = target;
    next.progress_percent = 100;
    set(next);
    util::logf("update: downloaded to %s", util::narrow(target).c_str());
    if (notify) PostMessageW(notify, WM_OMT_UPDATE, 1, 0);
}
