// A small control panel served over HTTP.
//
// Reliability decisions, all of them deliberate:
//
//   One thread. A single non-blocking select loop handles the listener and
//   every connection, so there are no per connection threads to leak, to
//   outlive the server, or to fail to create. Shutdown is a flag and a join.
//
//   Nothing a request touches belongs to the interface. The page and the state
//   are strings prepared on the interface thread and copied under a lock, and
//   commands are queued for the interface thread to apply. A request never
//   walks a window.
//
//   Bounded everywhere. Connections are capped, requests are capped, and every
//   connection has a deadline, so a client that stalls cannot accumulate.
//
// Off by default: it can close a window on a machine that is on air.
#pragma once
#include "util.h"

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>

struct WebWindow {
    int         id = 0;
    std::string kind;         // "viewer" or "multiview"
    std::string title;
    std::string address;
    int x = 0, y = 0, width = 0, height = 0;
    bool minimized = false;
    bool maximized = false;
};

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
    std::string state;
};

class WebServer {
public:
    ~WebServer() { stop(); }

    bool start(int port, HWND notify);
    void stop();
    bool running() const { return running_; }
    int  port() const { return port_; }
    std::string error() const;
    std::vector<std::string> urls() const;

    // Handed the finished state document by the interface thread. The socket
    // loop only ever copies it.
    void set_state(std::string json);

    // Drained by the interface thread when told there is work.
    bool take_commands(std::vector<WebCommand>* out);

private:
    struct Connection;

    void run();
    bool accept_one(std::vector<Connection>* into);
    bool on_readable(Connection& c);
    bool on_writable(Connection& c);
    std::string respond(const std::string& request);
    void queue(const WebCommand& command);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    uintptr_t         listener_ = ~uintptr_t(0);
    int               port_ = 0;
    HWND              notify_ = nullptr;

    mutable std::mutex state_mutex_;
    std::string        state_json_ = "{}";
    std::string        error_;

    mutable std::mutex     command_mutex_;
    std::deque<WebCommand> commands_;
};

WebServer& web_server();

// Builds the state document. Interface thread only: it reads the window list.
std::string build_web_state();
