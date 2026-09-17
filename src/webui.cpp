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

#include <iphlpapi.h>
#include <algorithm>

namespace {
WebServer g_server;

constexpr int kMaxRequestBytes = 64 * 1024;

std::string http_response(const std::string& status, const std::string& type,
                          const std::string& body) {
    std::string head = "HTTP/1.1 " + status + "\r\n";
    head += "Content-Type: " + type + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Connection: close\r\n\r\n";
    return head + body;
}

bool send_all(SOCKET client, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(client, data.data() + sent,
                           static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// The virtual desktop and each monitor within it, so the panel can draw a
// window where it actually is.
struct MonitorRect { int x, y, w, h; };

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(monitor, &mi)) {
        auto* out = reinterpret_cast<std::vector<MonitorRect>*>(param);
        out->push_back({ mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top });
    }
    return TRUE;
}

std::string build_state() {
    // ---- sources ----
    std::vector<std::string> source_objects;
    for (const auto& source : discovery().sources()) {
        const bool announced = source.is_manual && announcer().is_announced(source.address);
        source_objects.push_back(json::object({
            json::field("name", source.name),
            json::field("address", source.address),
            json::field("host", source.host),
            json::field("local", source.is_local),
            json::field("manual", source.is_manual),
            json::field("mini", source.is_omt_mini),
            json::field("mdns", announced),
            json::field("offline", source.status == SourceStatus::Offline),
        }));
    }

    // ---- windows ----
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

    // ---- desktop ----
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

    // ---- multiview ----
    const auto view = multiview_engine().snapshot();
    std::vector<std::string> tiles;
    for (const auto& address : view.assigned) tiles.push_back(json::str(address));

    wchar_t host[256] = {};
    DWORD host_len = ARRAYSIZE(host);
    GetComputerNameW(host, &host_len);

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
} // namespace

WebServer& web_server() { return g_server; }

std::vector<std::string> WebServer::urls() const {
    std::vector<std::string> out;
    if (!running_) return out;
    const std::string ip = netinfo::local_ipv4();
    if (!ip.empty()) out.push_back("http://" + ip + ":" + std::to_string(port_));
    out.push_back("http://localhost:" + std::to_string(port_));
    return out;
}

bool WebServer::start(int port) {
    if (running_) return true;
    error_.clear();

    if (!netinfo::init()) {
        error_ = "Winsock could not be started.";
        return false;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
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

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        error_ = "Port " + std::to_string(port) + " is already in use.";
        closesocket(listener);
        return false;
    }
    if (listen(listener, 8) == SOCKET_ERROR) {
        error_ = "The socket could not listen.";
        closesocket(listener);
        return false;
    }

    listener_ = static_cast<uintptr_t>(listener);
    port_ = port;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    util::logf("web: listening on port %d", port);
    return true;
}

void WebServer::stop() {
    if (!running_.exchange(false)) return;
    if (listener_ != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(listener_));
        listener_ = ~uintptr_t(0);
    }
    if (thread_.joinable()) thread_.join();
    util::logf("web: stopped");
}

void WebServer::queue(const WebCommand& command) {
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (commands_.size() > 64) commands_.pop_front();
        commands_.push_back(command);
    }
    // Applied on the interface thread: these move and destroy windows.
    if (HWND hwnd = App::instance().message_window())
        PostMessageW(hwnd, WM_OMT_WEBCMD, 0, 0);
}

bool WebServer::take_commands(std::vector<WebCommand>* out) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (commands_.empty()) return false;
    out->assign(commands_.begin(), commands_.end());
    commands_.clear();
    return true;
}

void WebServer::run() {
    while (running_) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        SOCKET client = accept(static_cast<SOCKET>(listener_),
                               reinterpret_cast<sockaddr*>(&from), &from_len);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            Sleep(20);
            continue;
        }
        // One short lived thread per request. A browser opens several
        // connections at once, and handling them in turn would stall the page.
        std::thread([this, client] { serve(static_cast<uintptr_t>(client)); }).detach();
    }
}

void WebServer::serve(uintptr_t client_handle) {
    const SOCKET client = static_cast<SOCKET>(client_handle);

    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));

    std::string request;
    char buffer[8192];
    size_t header_end = std::string::npos;

    while (request.size() < kMaxRequestBytes) {
        const int n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.append(buffer, static_cast<size_t>(n));

        if (header_end == std::string::npos) header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;

        // Keep reading until the declared body has arrived.
        size_t length = 0;
        const size_t at = request.find("Content-Length:");
        if (at != std::string::npos && at < header_end)
            length = static_cast<size_t>(std::atoi(request.c_str() + at + 15));
        if (request.size() >= header_end + 4 + length) break;
    }

    if (header_end == std::string::npos) {
        closesocket(client);
        return;
    }

    const size_t first_space = request.find(' ');
    const size_t second_space = request.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        closesocket(client);
        return;
    }
    const std::string method = request.substr(0, first_space);
    const std::string path = request.substr(first_space + 1, second_space - first_space - 1);
    const std::string body = request.substr(header_end + 4);

    std::string type = "application/json";
    const std::string payload = handle(method, path, body, &type);
    send_all(client, http_response("200 OK", type, payload));

    shutdown(client, SD_SEND);
    closesocket(client);
}

std::string WebServer::handle(const std::string& method, const std::string& path,
                              const std::string& body, std::string* content_type) {
    if (path == "/" || path == "/index.html") {
        *content_type = "text/html; charset=utf-8";
        return kControlPanelHtml;
    }

    if (path == "/api/state") return build_state();

    if (path == "/api/command" && method == "POST") {
        const std::string kind = json::get_string(body, "kind");

        WebCommand command;
        command.id      = json::get_int(body, "id");
        command.index   = json::get_int(body, "index");
        command.x       = json::get_int(body, "x");
        command.y       = json::get_int(body, "y");
        command.width   = json::get_int(body, "width");
        command.height  = json::get_int(body, "height");
        command.address = json::get_string(body, "address");
        command.name    = json::get_string(body, "name");
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

        if (command.kind == WebCommand::Kind::None)
            return json::object({ json::field("ok", false),
                                  json::field("message", "Unknown command") });

        queue(command);

        std::string message = "Done";
        if (command.kind == WebCommand::Kind::AddSource) {
            message = omt::has_explicit_port(command.address)
                    ? "Added"
                    : "Looking for senders on that machine";
        }
        return json::object({ json::field("ok", true), json::field("message", message) });
    }

    *content_type = "text/plain";
    return "Not found";
}
