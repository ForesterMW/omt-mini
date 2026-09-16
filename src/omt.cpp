#include "omt.h"

#include <algorithm>
#include <mutex>

namespace omt {
namespace {
HMODULE      g_module = nullptr;
Api          g_api{};
bool         g_loaded = false;
bool         g_tried = false;
std::wstring g_error;
std::mutex   g_mutex;

template <class T>
bool bind(T& slot, const char* name) {
    slot = reinterpret_cast<T>(
        reinterpret_cast<void*>(GetProcAddress(g_module, name)));
    if (!slot) {
        g_error = L"libomt.dll is missing the entry point '" + util::widen(name) +
                  L"'. It is probably an incompatible version.";
        return false;
    }
    return true;
}
} // namespace

bool load() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tried) return g_loaded;
    g_tried = true;

    // Prefer the copy shipped next to the executable so a system-wide OMT
    // install cannot silently swap the ABI underneath us.
    const std::wstring local = util::exe_dir() + L"\\libomt.dll";
    g_module = LoadLibraryW(local.c_str());
    if (!g_module) g_module = LoadLibraryW(L"libomt.dll");

    if (!g_module) {
        g_error = L"libomt.dll could not be loaded. Expected it next to OMTMini.exe at:\n"
                  + local;
        util::logf("omt: LoadLibrary failed (err=%lu)", GetLastError());
        return false;
    }

    const bool ok =
        bind(g_api.discovery_getaddresses,       "omt_discovery_getaddresses") &&
        bind(g_api.receive_create,               "omt_receive_create") &&
        bind(g_api.receive_destroy,              "omt_receive_destroy") &&
        bind(g_api.receive,                      "omt_receive") &&
        bind(g_api.receive_send,                 "omt_receive_send") &&
        bind(g_api.receive_settally,             "omt_receive_settally") &&
        bind(g_api.receive_gettally,             "omt_receive_gettally") &&
        bind(g_api.receive_setflags,             "omt_receive_setflags") &&
        bind(g_api.receive_setsuggestedquality,  "omt_receive_setsuggestedquality") &&
        bind(g_api.receive_getsenderinformation, "omt_receive_getsenderinformation") &&
        bind(g_api.receive_getvideostatistics,   "omt_receive_getvideostatistics") &&
        bind(g_api.receive_getaudiostatistics,   "omt_receive_getaudiostatistics") &&
        bind(g_api.send_create,                  "omt_send_create") &&
        bind(g_api.send_destroy,                 "omt_send_destroy") &&
        bind(g_api.send,                         "omt_send") &&
        bind(g_api.send_connections,             "omt_send_connections") &&
        bind(g_api.send_getaddress,              "omt_send_getaddress") &&
        bind(g_api.send_setsenderinformation,    "omt_send_setsenderinformation") &&
        bind(g_api.send_addconnectionmetadata,   "omt_send_addconnectionmetadata") &&
        bind(g_api.send_clearconnectionmetadata, "omt_send_clearconnectionmetadata") &&
        bind(g_api.send_setredirect,             "omt_send_setredirect") &&
        bind(g_api.send_receive,                 "omt_send_receive") &&
        bind(g_api.send_gettally,                "omt_send_gettally") &&
        bind(g_api.send_getvideostatistics,      "omt_send_getvideostatistics") &&
        bind(g_api.send_getaudiostatistics,      "omt_send_getaudiostatistics") &&
        bind(g_api.setloggingfilename,           "omt_setloggingfilename") &&
        bind(g_api.settings_get_string,          "omt_settings_get_string") &&
        bind(g_api.settings_set_string,          "omt_settings_set_string") &&
        bind(g_api.settings_get_integer,         "omt_settings_get_integer") &&
        bind(g_api.settings_set_integer,         "omt_settings_set_integer") &&
        bind(g_api.shutdown,                     "omt_shutdown");

    if (!ok) {
        FreeLibrary(g_module);
        g_module = nullptr;
        util::logf("omt: entry point binding failed");
        return false;
    }

    // Keep libomt's own log beside ours rather than in C:\ProgramData.
    const std::string libomt_log = util::narrow(util::config_path(L"libomt.log"));
    g_api.setloggingfilename(libomt_log.c_str());

    g_loaded = true;
    util::logf("omt: libomt.dll loaded");
    return true;
}

bool loaded() { return g_loaded; }
const std::wstring& load_error() { return g_error; }
const Api& api() { return g_api; }

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_loaded && g_api.shutdown) {
        g_api.shutdown();
        util::logf("omt: shutdown");
    }
    g_loaded = false;
}

const char* codec_name(OMTCodec c) {
    switch (c) {
        case OMTCodec_VMX1: return "VMX1";
        case OMTCodec_FPA1: return "FPA1";
        case OMTCodec_UYVY: return "UYVY";
        case OMTCodec_YUY2: return "YUY2";
        case OMTCodec_BGRA: return "BGRA";
        case OMTCodec_NV12: return "NV12";
        case OMTCodec_YV12: return "YV12";
        case OMTCodec_UYVA: return "UYVA";
        case OMTCodec_P216: return "P216";
        case OMTCodec_PA16: return "PA16";
        default: return "----";
    }
}

const char* quality_name(OMTQuality q) {
    switch (q) {
        case OMTQuality_Low:    return "Low";
        case OMTQuality_Medium: return "Medium";
        case OMTQuality_High:   return "High";
        default:                return "Default";
    }
}

// OMT advertises sources as "HOSTNAME (Source Name)".
std::string short_name(const std::string& address) {
    const size_t open = address.find(" (");
    if (open == std::string::npos) return address;
    if (address.size() && address.back() == ')')
        return address.substr(open + 2, address.size() - open - 3);
    return address.substr(open + 2);
}

std::string host_name(const std::string& address) {
    if (is_url_address(address)) {
        std::string rest = address.substr(6);
        const size_t slash = rest.find('/');
        if (slash != std::string::npos) rest = rest.substr(0, slash);
        return rest;
    }
    const size_t open = address.find(" (");
    if (open == std::string::npos) return {};
    return address.substr(0, open);
}

bool is_url_address(const std::string& address) {
    return address.size() > 6 && util::iequals(address.substr(0, 6), "omt://");
}

bool split_address(const std::string& address, std::string* host, std::string* port) {
    if (!is_url_address(address)) return false;

    std::string rest = address.substr(6);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    if (rest.empty()) return false;

    std::string h, p;
    if (rest[0] == '[') {
        const size_t close = rest.find(']');
        if (close == std::string::npos) return false;
        h = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':')
            p = rest.substr(close + 2);
    } else {
        const size_t colon = rest.rfind(':');
        if (colon == std::string::npos) {
            h = rest;
        } else {
            h = rest.substr(0, colon);
            p = rest.substr(colon + 1);
        }
    }

    if (h.empty()) return false;
    if (p.empty()) p = "6400";
    if (host) *host = h;
    if (port) *port = p;
    return true;
}

std::string host_only(const std::string& address) {
    std::string host, port;
    if (split_address(address, &host, &port)) return host;
    return host_name(address);
}

std::string safe_source_name(const std::string& name, const std::string& fallback) {
    auto clean = [](const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (unsigned char ch : in) {
            if (ch < 0x20 || ch == 0x7F) continue;      // control characters
            if (ch == '(' || ch == ')') continue;       // delimit HOST (NAME)
            // A dot ends a label in DNS-SD, and libomt does not escape them:
            // its own sanitiser has exactly this replacement sitting commented
            // out. A name carrying one is not announced at all.
            if (ch == '.' || ch == ':') { out += ' '; continue; }
            out += static_cast<char>(ch);
        }
        // Collapse runs of spaces left behind by the removals.
        std::string collapsed;
        bool space = false;
        for (char ch : out) {
            if (ch == ' ' || ch == '\t') {
                space = true;
                continue;
            }
            if (space && !collapsed.empty()) collapsed += ' ';
            space = false;
            collapsed += ch;
        }
        return util::trim(collapsed);
    };

    std::string result = clean(name);
    if (result.empty()) result = clean(fallback);
    if (result.empty()) result = "Source";

    // libomt truncates the whole HOSTNAME (NAME) string to fit its own limit,
    // which would cut a long name off mid word. Keep well clear of it.
    if (result.size() > 48) {
        result.resize(48);
        result = util::trim(result);
    }
    return result;
}

std::string default_direct_name(const std::vector<std::string>& taken) {
    for (int n = 1; n < 1000; ++n) {
        std::string candidate = "Direct source";
        if (n > 1) candidate += " " + std::to_string(n);
        const bool used = std::any_of(taken.begin(), taken.end(),
                                      [&](const std::string& other) {
                                          return util::iequals(other, candidate);
                                      });
        if (!used) return candidate;
    }
    return "Direct source";
}

bool has_explicit_port(const std::string& input) {
    std::string s = util::trim(input);
    if (s.size() > 6 && util::iequals(s.substr(0, 6), "omt://")) s = s.substr(6);
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    if (s.empty()) return false;

    if (s[0] == '[') {
        const size_t close = s.find(']');
        return close != std::string::npos && close + 1 < s.size() && s[close + 1] == ':';
    }
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    // Several colons is a bare IPv6 literal, not a host and port.
    return s.find(':', colon + 1) == std::string::npos;
}

std::string normalize_address(const std::string& input, int default_port) {
    std::string s = util::trim(input);
    if (s.empty()) return {};

    // Strip a scheme if one was typed, then put the canonical one back.
    if (s.size() > 6 && util::iequals(s.substr(0, 6), "omt://")) s = s.substr(6);
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    s = util::trim(s);
    if (s.empty()) return {};

    // A bracketed IPv6 literal keeps its brackets; the port is what follows
    // the closing one.
    bool has_port = false;
    if (s[0] == '[') {
        const size_t close = s.find(']');
        if (close == std::string::npos) return {};
        has_port = (close + 1 < s.size() && s[close + 1] == ':');
    } else {
        const size_t colon = s.find(':');
        // More than one colon means a bare IPv6 literal, which needs brackets
        // before a port can be appended unambiguously.
        if (colon != std::string::npos && s.find(':', colon + 1) != std::string::npos) {
            s = "[" + s + "]";
            has_port = false;
        } else {
            has_port = (colon != std::string::npos);
        }
    }

    if (!has_port) {
        s += ":";
        s += std::to_string(default_port > 0 ? default_port : 6400);
    }
    return "omt://" + s;
}

// ---- Receiver ----------------------------------------------------------
bool Receiver::open(const std::string& address, OMTFrameType types,
                    OMTPreferredVideoFormat format, OMTReceiveFlags flags) {
    close();
    if (!g_loaded) return false;
    inst_ = g_api.receive_create(address.c_str(), types, format, flags);
    if (!inst_) {
        util::logf("omt: receive_create failed for '%s'", address.c_str());
        return false;
    }
    util::logf("omt: receiving '%s'", address.c_str());
    return true;
}

void Receiver::close() {
    if (inst_) {
        g_api.receive_destroy(inst_);
        inst_ = nullptr;
    }
}

OMTMediaFrame* Receiver::receive(OMTFrameType types, int timeout_ms) {
    if (!inst_) return nullptr;
    return g_api.receive(inst_, types, timeout_ms);
}

void Receiver::set_tally(bool preview, bool program) {
    if (!inst_) return;
    OMTTally t{};
    t.preview = preview ? 1 : 0;
    t.program = program ? 1 : 0;
    g_api.receive_settally(inst_, &t);
}

bool Receiver::get_tally(int timeout_ms, OMTTally* out) {
    if (!inst_) return false;
    return g_api.receive_gettally(inst_, timeout_ms, out) != 0;
}

void Receiver::set_flags(OMTReceiveFlags flags) {
    if (inst_) g_api.receive_setflags(inst_, flags);
}

void Receiver::set_suggested_quality(OMTQuality q) {
    if (inst_) g_api.receive_setsuggestedquality(inst_, q);
}

bool Receiver::sender_info(OMTSenderInfo* out) {
    if (!inst_ || !out) return false;
    *out = OMTSenderInfo{};
    g_api.receive_getsenderinformation(inst_, out);
    return out->ProductName[0] != '\0';
}

void Receiver::video_stats(OMTStatistics* out) {
    if (inst_ && out) g_api.receive_getvideostatistics(inst_, out);
}

void Receiver::audio_stats(OMTStatistics* out) {
    if (inst_ && out) g_api.receive_getaudiostatistics(inst_, out);
}

bool Receiver::send_metadata(const std::string& xml) {
    if (!inst_) return false;
    OMTMediaFrame f{};
    f.Type = OMTFrameType_Metadata;
    f.Data = const_cast<char*>(xml.c_str());
    f.DataLength = static_cast<int>(xml.size()) + 1;
    return g_api.receive_send(inst_, &f) != 0;
}

// ---- Sender ------------------------------------------------------------
bool Sender::open(const std::string& name, OMTQuality quality) {
    close();
    if (!g_loaded) return false;
    inst_ = g_api.send_create(name.c_str(), quality);
    if (!inst_) {
        util::logf("omt: send_create failed for '%s'", name.c_str());
        return false;
    }
    util::logf("omt: sending as '%s'", name.c_str());
    return true;
}

void Sender::close() {
    if (inst_) {
        g_api.send_destroy(inst_);
        inst_ = nullptr;
    }
}

int Sender::send(OMTMediaFrame* frame) {
    if (!inst_) return 0;
    return g_api.send(inst_, frame);
}

int Sender::connections() {
    return inst_ ? g_api.send_connections(inst_) : 0;
}

std::string Sender::address() {
    if (!inst_) return {};
    char buf[OMT_MAX_STRING_LENGTH] = {};
    const int n = g_api.send_getaddress(inst_, buf, sizeof(buf));
    if (n <= 0) return {};
    return std::string(buf);
}

void Sender::set_sender_info(const char* product, const char* manufacturer, const char* version) {
    if (!inst_) return;
    OMTSenderInfo info{};
    lstrcpynA(info.ProductName,  product,      OMT_MAX_STRING_LENGTH);
    lstrcpynA(info.Manufacturer, manufacturer, OMT_MAX_STRING_LENGTH);
    lstrcpynA(info.Version,      version,      OMT_MAX_STRING_LENGTH);
    g_api.send_setsenderinformation(inst_, &info);
}

void Sender::set_redirect(const std::string& address) {
    if (!inst_) return;
    g_api.send_setredirect(inst_, address.empty() ? nullptr : address.c_str());
}

bool Sender::get_tally(int timeout_ms, OMTTally* out) {
    if (!inst_) return false;
    return g_api.send_gettally(inst_, timeout_ms, out) != 0;
}

void Sender::video_stats(OMTStatistics* out) {
    if (inst_ && out) g_api.send_getvideostatistics(inst_, out);
}

} // namespace omt
