// Runtime binding to libomt.dll (Open Media Transport).
//
// libomt.dll is loaded with LoadLibrary rather than linked against, for two
// reasons: OMT Mini stays launchable when the DLL is missing (so it can show a
// readable error instead of failing at process start), and the user can point
// the app at a newer libomt without us shipping a new import library.
#pragma once
#include "util.h"
#include <string>
#include <vector>

extern "C" {
#include "libomt.h"
}

namespace omt {

// ---- entry points ------------------------------------------------------
// Names mirror libomt.h exactly so the header stays the reference.
using fn_discovery_getaddresses    = char**            (*)(int*);
using fn_receive_create            = omt_receive_t*    (*)(const char*, OMTFrameType, OMTPreferredVideoFormat, OMTReceiveFlags);
using fn_receive_destroy           = void              (*)(omt_receive_t*);
using fn_receive                   = OMTMediaFrame*    (*)(omt_receive_t*, OMTFrameType, int);
using fn_receive_send              = int               (*)(omt_receive_t*, OMTMediaFrame*);
using fn_receive_settally          = void              (*)(omt_receive_t*, OMTTally*);
using fn_receive_gettally          = int               (*)(omt_receive_t*, int, OMTTally*);
using fn_receive_setflags          = void              (*)(omt_receive_t*, OMTReceiveFlags);
using fn_receive_setsuggestedquality = void            (*)(omt_receive_t*, OMTQuality);
using fn_receive_getsenderinformation = void           (*)(omt_receive_t*, OMTSenderInfo*);
using fn_receive_getvideostatistics = void             (*)(omt_receive_t*, OMTStatistics*);
using fn_receive_getaudiostatistics = void             (*)(omt_receive_t*, OMTStatistics*);
using fn_send_create               = omt_send_t*       (*)(const char*, OMTQuality);
using fn_send_destroy              = void              (*)(omt_send_t*);
using fn_send                      = int               (*)(omt_send_t*, OMTMediaFrame*);
using fn_send_connections          = int               (*)(omt_send_t*);
using fn_send_getaddress           = int               (*)(omt_send_t*, char*, int);
using fn_send_setsenderinformation = void              (*)(omt_send_t*, OMTSenderInfo*);
using fn_send_addconnectionmetadata = void             (*)(omt_send_t*, const char*);
using fn_send_clearconnectionmetadata = void           (*)(omt_send_t*);
using fn_send_setredirect          = void              (*)(omt_send_t*, const char*);
using fn_send_receive              = OMTMediaFrame*    (*)(omt_send_t*, int);
using fn_send_gettally             = int               (*)(omt_send_t*, int, OMTTally*);
using fn_send_getvideostatistics   = void              (*)(omt_send_t*, OMTStatistics*);
using fn_send_getaudiostatistics   = void              (*)(omt_send_t*, OMTStatistics*);
using fn_setloggingfilename        = void              (*)(const char*);
using fn_settings_get_string       = int               (*)(const char*, char*, int);
using fn_settings_set_string       = void              (*)(const char*, const char*);
using fn_settings_get_integer      = int               (*)(const char*);
using fn_settings_set_integer      = void              (*)(const char*, int);
using fn_shutdown                  = void              (*)();

struct Api {
    fn_discovery_getaddresses         discovery_getaddresses = nullptr;
    fn_receive_create                 receive_create = nullptr;
    fn_receive_destroy                receive_destroy = nullptr;
    fn_receive                        receive = nullptr;
    fn_receive_send                   receive_send = nullptr;
    fn_receive_settally               receive_settally = nullptr;
    fn_receive_gettally               receive_gettally = nullptr;
    fn_receive_setflags               receive_setflags = nullptr;
    fn_receive_setsuggestedquality    receive_setsuggestedquality = nullptr;
    fn_receive_getsenderinformation   receive_getsenderinformation = nullptr;
    fn_receive_getvideostatistics     receive_getvideostatistics = nullptr;
    fn_receive_getaudiostatistics     receive_getaudiostatistics = nullptr;
    fn_send_create                    send_create = nullptr;
    fn_send_destroy                   send_destroy = nullptr;
    fn_send                           send = nullptr;
    fn_send_connections               send_connections = nullptr;
    fn_send_getaddress                send_getaddress = nullptr;
    fn_send_setsenderinformation      send_setsenderinformation = nullptr;
    fn_send_addconnectionmetadata     send_addconnectionmetadata = nullptr;
    fn_send_clearconnectionmetadata   send_clearconnectionmetadata = nullptr;
    fn_send_setredirect               send_setredirect = nullptr;
    fn_send_receive                   send_receive = nullptr;
    fn_send_gettally                  send_gettally = nullptr;
    fn_send_getvideostatistics        send_getvideostatistics = nullptr;
    fn_send_getaudiostatistics        send_getaudiostatistics = nullptr;
    fn_setloggingfilename             setloggingfilename = nullptr;
    fn_settings_get_string            settings_get_string = nullptr;
    fn_settings_set_string            settings_set_string = nullptr;
    fn_settings_get_integer           settings_get_integer = nullptr;
    fn_settings_set_integer           settings_set_integer = nullptr;
    fn_shutdown                       shutdown = nullptr;
};

// Loads libomt.dll from the executable directory, then the default search
// path. Safe to call repeatedly; only the first call does work.
bool load();
bool loaded();
// Human readable reason the last load() failed, for the UI to show.
const std::wstring& load_error();
const Api& api();
// Releases discovery/logging threads. Call once, after every sender and
// receiver has been destroyed.
void shutdown();

// ---- helpers -----------------------------------------------------------
const char* codec_name(OMTCodec c);
const char* quality_name(OMTQuality q);
// "HOSTNAME (Source Name)" -> "Source Name", for compact display.
std::string short_name(const std::string& address);
std::string host_name(const std::string& address);
// True for a direct omt://host:port address rather than a discovered name.
bool is_url_address(const std::string& address);
// Turns what someone typed into an address libomt accepts. Accepts
// "10.0.0.5", "10.0.0.5:6400", "host.local", "omt://host:6400" and IPv6 in
// brackets. Returns an empty string if it cannot be made sense of.
std::string normalize_address(const std::string& input, int default_port = 6400);
// Splits omt://host:port into its parts. IPv6 literals come back without their
// brackets, which is what getaddrinfo wants. False if it is not a URL address.
bool split_address(const std::string& address, std::string* host, std::string* port);
// True when what someone typed already carried a port, so it should be taken
// literally rather than scanned for.
bool has_explicit_port(const std::string& input);

// ---- RAII wrappers -----------------------------------------------------
class Receiver {
public:
    Receiver() = default;
    ~Receiver() { close(); }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    bool open(const std::string& address, OMTFrameType types,
              OMTPreferredVideoFormat format, OMTReceiveFlags flags);
    void close();
    bool is_open() const { return inst_ != nullptr; }

    // Valid until the next receive() call for the same frame type.
    OMTMediaFrame* receive(OMTFrameType types, int timeout_ms);

    void set_tally(bool preview, bool program);
    bool get_tally(int timeout_ms, OMTTally* out);
    void set_flags(OMTReceiveFlags flags);
    void set_suggested_quality(OMTQuality q);
    bool sender_info(OMTSenderInfo* out);
    void video_stats(OMTStatistics* out);
    void audio_stats(OMTStatistics* out);
    bool send_metadata(const std::string& xml);

private:
    omt_receive_t* inst_ = nullptr;
};

class Sender {
public:
    Sender() = default;
    ~Sender() { close(); }
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    bool open(const std::string& name, OMTQuality quality);
    void close();
    bool is_open() const { return inst_ != nullptr; }

    int  send(OMTMediaFrame* frame);
    int  connections();
    std::string address();
    void set_sender_info(const char* product, const char* manufacturer, const char* version);
    // Tells anything that connects to go and get the media from somewhere else.
    // This is what OMT calls a virtual source: the sender is announced on the
    // network but carries nothing itself.
    void set_redirect(const std::string& address);
    bool get_tally(int timeout_ms, OMTTally* out);
    void video_stats(OMTStatistics* out);

private:
    omt_send_t* inst_ = nullptr;
};

} // namespace omt
