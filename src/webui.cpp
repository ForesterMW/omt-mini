// Winsock first: it has to precede anything that reaches windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "webui.h"
#include "webpage.h"
#include "json.h"
#include "app.h"
#include "discovery.h"
#include "announce.h"
#include "settings.h"
#include "netinfo.h"
#include "multiview.h"
#include "omt.h"

#include <algorithm>

namespace {
WebServer g_server;

constexpr int    kMaxConnections   = 24;
constexpr size_t kMaxRequestBytes  = 64 * 1024;
constexpr int64_t kConnectionLifeMs = 15000;
constexpr int    kSelectTimeoutMs  = 100;

std::string http_response(const char* status, const char* type, const std::string& body) {
    std::string head = "HTTP/1.1 ";
    head += status;
    head += "\r\nContent-Type: ";
    head += type;
    head += "\r\nContent-Length: " + std::to_string(body.size());
    head += "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    return head + body;
}

struct MonitorRect { int x, y, w, h; };

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(monitor, &mi)) {
        reinterpret_cast<std::vector<MonitorRect>*>(param)->push_back(
            { mi.rcMonitor.left, mi.rcMonitor.top,
              mi.rcMonitor.right - mi.rcMonitor.left,
              mi.rcMonitor.bottom - mi.rcMonitor.top });
    }
    return TRUE;
}
} // namespace

// One connection, read then written then closed. No keep alive: a control
// panel makes a handful of small requests and simplicity is worth more here
// than a saved handshake.
struct WebServer::Connection {
    uintptr_t   socket = ~uintptr_t(0);
    std::string in;
    std::string out;
    size_t      sent = 0;
    bool        writing = false;
    int64_t     deadline = 0;
};

WebServer& web_server() { return g_server; }

// ---- the state document, built on the interface thread ------------------
std::string build_web_state() {
    std::vector<std::string> source_objects;
    for (const auto& source : discovery().sources()) {
        source_objects.push_back(json::object({
            json::field("name", source.name),
            json::field("address", source.address),
            json::field("host", source.host),
            json::field("local", source.is_local),
            json::field("manual", source.is_manual),
            json::field("mini", source.is_omt_mini),
            json::field("mdns", source.is_manual && announcer().is_announced(source.address)),
            json::field("offline", source.status == SourceStatus::Offline),
        }));
    }

    std::vector<std::string> window_objects;
    for (const auto& window : App::instance().web_windows()) {
        window_objects.push_back(json::object({
            json::field("id", window.id),
            json::field("kind", window.kind),
            json::field("title", window.title),
            json::field("address", window.address),
            json::field("x", window.x),
            json::field("y", window.y),
            json::field("width", window.width),
            json::field("height", window.height),
            json::field("minimized", window.minimized),
            json::field("maximized", window.maximized),
        }));
    }

    std::vector<MonitorRect> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &collect_monitor,
                        reinterpret_cast<LPARAM>(&monitors));
    std::vector<std::string> monitor_objects;
    for (const auto& m : monitors) {
        monitor_objects.push_back(json::object({
            json::field("x", m.x), json::field("y", m.y),
            json::field("width", m.w), json::field("height", m.h),
        }));
    }

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(640, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = std::max(480, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    const auto view = multiview_engine().snapshot();
    std::vector<std::string> tiles;
    for (const auto& address : view.assigned) tiles.push_back(json::str(address));

    wchar_t host[256] = {};
    DWORD host_len = ARRAYSIZE(host);
    if (!GetComputerNameW(host, &host_len)) host_len = 0;

    return json::object({
        json::field("host", util::narrow(std::wstring(host, host_len))),
        json::str("sources") + ":" + json::array(source_objects),
        json::str("windows") + ":" + json::array(window_objects),
        json::str("monitors") + ":" + json::array(monitor_objects),
        json::str("desktop") + ":" + json::object({
            json::field("x", vx), json::field("y", vy),
            json::field("width", vw), json::field("height", vh),
        }),
        json::field("multiview_layout", view.layout),
        json::str("multiview_tiles") + ":" + json::array(tiles),
        json::field("multiview_open", multiview_engine().active()),
    });
}

void WebServer::set_state(std::string json_text) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_json_.swap(json_text);
}

std::string WebServer::error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return error_;
}

std::vector<std::string> WebServer::urls() const {
    std::vector<std::string> out;
    if (!running_) return out;
    const std::string ip = netinfo::local_ipv4();
    if (!ip.empty()) out.push_back("http://" + ip + ":" + std::to_string(port_));
    out.push_back("http://localhost:" + std::to_string(port_));
    return out;
}

// ---- lifecycle ----------------------------------------------------------
bool WebServer::start(int port, HWND notify) {
    if (running_) return true;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        error_.clear();
    }

    if (!netinfo::init()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        error_ = "Winsock could not be started.";
        return false;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        error_ = "The listening socket could not be created.";
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 16) == SOCKET_ERROR) {
        closesocket(listener);
        std::lock_guard<std::mutex> lock(state_mutex_);
        error_ = "Port " + std::to_string(port) + " could not be opened. It may be in use.";
        return false;
    }

    u_long nonblocking = 1;
    ioctlsocket(listener, FIONBIO, &nonblocking);

    listener_ = static_cast<uintptr_t>(listener);
    port_ = port;
    notify_ = notify;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    util::logf("web: listening on port %d", port);
    return true;
}

void WebServer::stop() {
    if (!running_.exchange(false)) return;
    // The loop wakes at least every hundred milliseconds and closes everything
    // itself, so there is nothing to interrupt and nothing to leak.
    if (thread_.joinable()) thread_.join();
    util::logf("web: stopped");
}

void WebServer::queue(const WebCommand& command) {
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        // Bounded: a client that floods commands cannot grow this without end.
        while (commands_.size() >= 128) commands_.pop_front();
        commands_.push_back(command);
    }
    if (notify_) PostMessageW(notify_, WM_OMT_WEBCMD, 0, 0);
}

bool WebServer::take_commands(std::vector<WebCommand>* out) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (commands_.empty()) return false;
    out->assign(commands_.begin(), commands_.end());
    commands_.clear();
    return true;
}

// ---- the loop -----------------------------------------------------------
void WebServer::run() {
    std::vector<Connection> connections;

    auto drop = [&connections](size_t index) {
        closesocket(static_cast<SOCKET>(connections[index].socket));
        connections.erase(connections.begin() + index);
    };

    while (running_) {
        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(static_cast<SOCKET>(listener_), &readable);

        for (const auto& c : connections) {
            if (c.writing) FD_SET(static_cast<SOCKET>(c.socket), &writable);
            else           FD_SET(static_cast<SOCKET>(c.socket), &readable);
        }

        timeval timeout{ 0, kSelectTimeoutMs * 1000 };
        const int ready = select(0, &readable, &writable, nullptr, &timeout);
        if (!running_) break;
        if (ready == SOCKET_ERROR) {
            // Nothing recoverable to do per error here; pause briefly rather
            // than spin, and let the next pass try again.
            Sleep(20);
            continue;
        }

        if (FD_ISSET(static_cast<SOCKET>(listener_), &readable)) {
            // Drain the backlog, but never past the cap: a client that opens
            // connections faster than they finish cannot grow this list.
            while (connections.size() < kMaxConnections && accept_one(&connections)) {}
        }

        const int64_t now = util::now_ms();
        for (size_t i = connections.size(); i-- > 0;) {
            auto& c = connections[i];
            bool finished = false;

            if (c.writing) {
                if (FD_ISSET(static_cast<SOCKET>(c.socket), &writable))
                    finished = on_writable(c);
            } else if (FD_ISSET(static_cast<SOCKET>(c.socket), &readable)) {
                finished = on_readable(c);
            }

            // A connection that stalls is dropped rather than kept for ever.
            if (finished || now > c.deadline) drop(i);
        }
    }

    for (auto& c : connections) closesocket(static_cast<SOCKET>(c.socket));
    if (listener_ != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(listener_));
        listener_ = ~uintptr_t(0);
    }
}

bool WebServer::accept_one(std::vector<Connection>* into) {
    SOCKET client = accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
    if (client == INVALID_SOCKET) return false;

    u_long nonblocking = 1;
    ioctlsocket(client, FIONBIO, &nonblocking);

    Connection c;
    c.socket = static_cast<uintptr_t>(client);
    c.deadline = util::now_ms() + kConnectionLifeMs;
    into->push_back(std::move(c));
    return true;
}

// Returns true when the connection is done with.
bool WebServer::on_readable(Connection& c) {
    char buffer[8192];
    for (;;) {
        const int n = recv(static_cast<SOCKET>(c.socket), buffer, sizeof(buffer), 0);
        if (n > 0) {
            if (c.in.size() + static_cast<size_t>(n) > kMaxRequestBytes) return true;
            c.in.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return true;                       // closed by the client
        if (WSAGetLastError() == WSAEWOULDBLOCK) break; // nothing more for now
        return true;                                   // any real error
    }

    const size_t header_end = c.in.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;  // headers still arriving

    size_t length = 0;
    const size_t at = c.in.find("Content-Length:");
    if (at != std::string::npos && at < header_end)
        length = static_cast<size_t>(std::atoi(c.in.c_str() + at + 15));
    if (c.in.size() < header_end + 4 + length) return false;   // body still arriving

    c.out = respond(c.in);
    c.sent = 0;
    c.writing = true;
    return false;
}

bool WebServer::on_writable(Connection& c) {
    while (c.sent < c.out.size()) {
        const int n = send(static_cast<SOCKET>(c.socket), c.out.data() + c.sent,
                           static_cast<int>(c.out.size() - c.sent), 0);
        if (n > 0) { c.sent += static_cast<size_t>(n); continue; }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return false;
        return true;
    }
    shutdown(static_cast<SOCKET>(c.socket), SD_SEND);
    return true;
}

std::string WebServer::respond(const std::string& request) {
    const size_t first = request.find(' ');
    const size_t second = request.find(' ', first + 1);
    if (first == std::string::npos || second == std::string::npos)
        return http_response("400 Bad Request", "text/plain", "Bad request");

    const std::string method = request.substr(0, first);
    const std::string path = request.substr(first + 1, second - first - 1);

    if (path == "/" || path == "/index.html")
        return http_response("200 OK", "text/html; charset=utf-8", kControlPanelHtml);

    if (path == "/api/state") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return http_response("200 OK", "application/json", state_json_);
    }

    if (path == "/api/command" && method == "POST") {
        const size_t header_end = request.find("\r\n\r\n");
        const std::string body =
            header_end == std::string::npos ? std::string() : request.substr(header_end + 4);

        const std::string kind = json::get_string(body, "kind");

        WebCommand command;
        command.id      = json::get_int(body, "id");
        command.index   = json::get_int(body, "index");
        command.x       = json::get_int(body, "x");
        command.y       = json::get_int(body, "y");
        command.width   = json::get_int(body, "width");
        command.height  = json::get_int(body, "height");
        command.address = json::get_string(body, "address");
        command.state   = json::get_string(body, "state");

        if (kind == "source")      command.kind = WebCommand::Kind::SetSource;
        else if (kind == "move")   command.kind = WebCommand::Kind::Move;
        else if (kind == "state")  command.kind = WebCommand::Kind::WindowState;
        else if (kind == "open")   command.kind = WebCommand::Kind::OpenViewer;
        else if (kind == "close")  command.kind = WebCommand::Kind::CloseWindow;
        else if (kind == "tile")   command.kind = WebCommand::Kind::MultiviewTile;
        else if (kind == "layout") command.kind = WebCommand::Kind::MultiviewLayout;
        else if (kind == "openmv") command.kind = WebCommand::Kind::OpenMultiview;
        else if (kind == "add")    command.kind = WebCommand::Kind::AddSource;

        if (command.kind == WebCommand::Kind::None) {
            return http_response("200 OK", "application/json",
                json::object({ json::field("ok", false),
                               json::field("message", "Unknown command") }));
        }

        // Sanity, before anything is asked of a window. Geometry arrives from a
        // browser and must never be trusted to be sensible.
        if (command.kind == WebCommand::Kind::Move) {
            // Upper bound well below anything that would ask for a swap chain
            // the machine cannot allocate.
            command.width  = std::clamp(command.width, kMinWindowWidth, 8192);
            command.height = std::clamp(command.height, kMinWindowHeight, 8192);
            command.x      = std::clamp(command.x, -32768, 32768);
            command.y      = std::clamp(command.y, -32768, 32768);
        }
        command.index = std::clamp(command.index, 0, 63);

        queue(command);

        const std::string message =
            command.kind == WebCommand::Kind::AddSource
                ? (omt::has_explicit_port(command.address)
                       ? "Added" : "Looking for senders on that machine")
                : "Done";
        return http_response("200 OK", "application/json",
            json::object({ json::field("ok", true), json::field("message", message) }));
    }

    return http_response("404 Not Found", "text/plain", "Not found");
}
