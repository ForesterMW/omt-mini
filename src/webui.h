// A small control panel served over HTTP.
//
// Lists the viewers and the multiview this copy has open, lets their source be
// changed, moved, resized, maximised or closed from a browser on another
// machine, and lets the multiview be laid out by dragging sources onto it.
//
// Off by default. It can close a window on a machine that is on air, so it is
// not something to start listening on a network without being asked.
#pragma once
#include "util.h"

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>

// One controllable window, as the panel sees it.
struct WebWindow {
    int         id = 0;
    std::string kind;         // "viewer" or "multiview"
    std::string title;
    std::string address;      // the viewer's source, empty for the multiview
    int x = 0, y = 0, width = 0, height = 0;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
};

// Everything the panel needs in one round trip.
struct WebState {
    std::vector<WebWindow> windows;
    // Virtual desktop bounds, so the panel can draw windows where they are.
    int desktop_x = 0, desktop_y = 0, desktop_width = 0, desktop_height = 0;
    int multiview_layout = 0;
    std::vector<std::string> multiview_tiles;
    bool multiview_open = false;
    int  revision = 0;
};

// A change asked for by the panel. Applied on the interface thread, never on
// the socket thread: these move and destroy windows.
struct WebCommand {
    enum class Kind {
        None, SetSource, Move, WindowState, OpenViewer, CloseWindow,
        MultiviewTile, MultiviewLayout, OpenMultiview, AddSource,
    };
    Kind        kind = Kind::None;
    int         id = 0;
    int         index = 0;
    int         x = 0, y = 0, width = 0, height = 0;
    std::string address;
    std::string name;
    std::string state;        // "maximize" | "restore" | "minimize"
};

class WebServer {
public:
    ~WebServer() { stop(); }

    bool start(int port);
    void stop();
    bool running() const { return running_; }
    int  port() const { return port_; }
    const std::string& error() const { return error_; }
    // Addresses the panel can be reached on, for showing in settings.
    std::vector<std::string> urls() const;

    // Drained by the interface thread when it is told there is work.
    bool take_commands(std::vector<WebCommand>* out);

private:
    void run();
    void serve(uintptr_t client);
    std::string handle(const std::string& method, const std::string& path,
                       const std::string& body, std::string* content_type);
    void queue(const WebCommand& command);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    uintptr_t         listener_ = ~uintptr_t(0);
    int               port_ = 0;
    std::string       error_;

    mutable std::mutex     command_mutex_;
    std::deque<WebCommand> commands_;
};

WebServer& web_server();
