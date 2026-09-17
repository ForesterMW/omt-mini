// Just enough JSON for the control panel API.
//
// Output is built by hand because the shapes are fixed and small. Input is
// scanned for named fields rather than parsed into a tree, for the same reason
// the updater does it: four fields do not justify a dependency.
#pragma once
#include <string>
#include <vector>
#include <cstdlib>

namespace json {

inline std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char ch : in) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}

inline std::string str(const std::string& value) { return "\"" + escape(value) + "\""; }

inline std::string field(const std::string& key, const std::string& value) {
    return str(key) + ":" + str(value);
}
// Without this a string literal binds to the bool overload instead: pointer to
// bool is a standard conversion and beats the user defined one to std::string,
// so every literal would silently serialise as true.
inline std::string field(const std::string& key, const char* value) {
    return str(key) + ":" + str(value ? value : "");
}
inline std::string field(const std::string& key, int value) {
    return str(key) + ":" + std::to_string(value);
}
inline std::string field(const std::string& key, bool value) {
    return str(key) + ":" + (value ? "true" : "false");
}
inline std::string object(const std::vector<std::string>& fields) {
    std::string out = "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out += ",";
        out += fields[i];
    }
    return out + "}";
}
inline std::string array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += items[i];
    }
    return out + "]";
}

// ---- reading ----
inline std::string unescape(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        switch (in[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case '/': out += '/';  break;
            default: out += in[i]; break;
        }
    }
    return out;
}

// Finds where a named field's value starts.
//
// The name has to be followed by a colon to count. Searching for the quoted
// name alone matches it as a VALUE too, which is not hypothetical: the panel
// posts {"kind":"state", ... ,"state":"maximize"}, and looking for "state"
// found the value of kind first, so every maximise silently became a restore.
inline size_t find_value(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        size_t after = pos + needle.size();
        while (after < body.size() && isspace(static_cast<unsigned char>(body[after])))
            ++after;
        if (after < body.size() && body[after] == ':') {
            ++after;
            while (after < body.size() && isspace(static_cast<unsigned char>(body[after])))
                ++after;
            return after;
        }
        pos += needle.size();
    }
    return std::string::npos;
}

inline std::string get_string(const std::string& body, const std::string& key) {
    size_t pos = find_value(body, key);
    if (pos == std::string::npos || pos >= body.size() || body[pos] != '"') return {};
    ++pos;

    std::string raw;
    while (pos < body.size()) {
        if (body[pos] == '\\' && pos + 1 < body.size()) {
            raw += body[pos];
            raw += body[pos + 1];
            pos += 2;
            continue;
        }
        if (body[pos] == '"') break;
        raw += body[pos++];
    }
    return unescape(raw);
}

inline int get_int(const std::string& body, const std::string& key, int fallback = 0) {
    const size_t pos = find_value(body, key);
    if (pos == std::string::npos || pos >= body.size()) return fallback;
    if (body[pos] != '-' && !isdigit(static_cast<unsigned char>(body[pos]))) return fallback;
    return std::atoi(body.c_str() + pos);
}

inline bool get_bool(const std::string& body, const std::string& key, bool fallback = false) {
    const size_t pos = find_value(body, key);
    if (pos == std::string::npos) return fallback;
    if (body.compare(pos, 4, "true") == 0) return true;
    if (body.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

} // namespace json
