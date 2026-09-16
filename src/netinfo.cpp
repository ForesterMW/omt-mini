// Winsock first: it has to precede anything that reaches windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "netinfo.h"
#include "util.h"

#include <algorithm>
#include <vector>

namespace netinfo {
namespace {
bool g_ready = false;
}

bool init() {
    if (g_ready) return true;
    WSADATA wsa{};
    g_ready = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    if (!g_ready) util::logf("netinfo: WSAStartup failed");
    return g_ready;
}

void shutdown() {
    if (!g_ready) return;
    WSACleanup();
    g_ready = false;
}

std::string local_ipv4() {
    if (!g_ready && !init()) return {};

    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;

    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                        &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                      &size);
    }
    if (result != NO_ERROR) return {};

    // Prefer an adapter with a gateway: on a machine with virtual switches and
    // VPN adapters that is the one another box on the network can reach.
    std::string fallback;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr) continue;
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;

            char text[INET_ADDRSTRLEN] = {};
            auto* in4 = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            if (!inet_ntop(AF_INET, &in4->sin_addr, text, sizeof(text))) continue;
            if (std::string(text).rfind("169.254.", 0) == 0) continue;   // link local

            if (adapter->FirstGatewayAddress) return text;
            if (fallback.empty()) fallback = text;
        }
    }
    return fallback;
}

std::string resolve(const std::string& host) {
    if (host.empty()) return {};
    if (!g_ready && !init()) return {};

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) return {};

    std::string text;
    // Prefer IPv4: it is what someone typing an address by hand will want.
    for (int pass = 0; pass < 2 && text.empty(); ++pass) {
        const int want = pass == 0 ? AF_INET : AF_INET6;
        for (addrinfo* ai = found; ai; ai = ai->ai_next) {
            if (ai->ai_family != want) continue;
            char buffer[INET6_ADDRSTRLEN] = {};
            if (want == AF_INET) {
                auto* in4 = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
                inet_ntop(AF_INET, &in4->sin_addr, buffer, sizeof(buffer));
            } else {
                auto* in6 = reinterpret_cast<sockaddr_in6*>(ai->ai_addr);
                inet_ntop(AF_INET6, &in6->sin6_addr, buffer, sizeof(buffer));
            }
            if (buffer[0]) { text = buffer; break; }
        }
    }
    freeaddrinfo(found);
    return text;
}

bool port_open(const std::string& host, const std::string& port, int timeout_ms) {
    if (!g_ready && !init()) return false;

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* resolved = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved) != 0 || !resolved)
        return false;

    bool reachable = false;
    for (addrinfo* ai = resolved; ai && !reachable; ai = ai->ai_next) {
        SOCKET sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        u_long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);

        if (connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            reachable = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable); FD_SET(sock, &writable);
            FD_ZERO(&failed);   FD_SET(sock, &failed);
            timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            if (select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(sock, &writable)) {
                int error = 0;
                int len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&error), &len) == 0 && error == 0)
                    reachable = true;
            }
        }
        closesocket(sock);
    }
    freeaddrinfo(resolved);
    return reachable;
}

std::vector<int> listening_ports(int low, int high) {
    std::vector<int> ports;
    if (!g_ready && !init()) return ports;

    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return ports;

    std::vector<uint8_t> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return ports;

    const DWORD pid = GetCurrentProcessId();
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwOwningPid != pid) continue;
        const int port = static_cast<int>(ntohs(static_cast<u_short>(row.dwLocalPort)));
        if (port < low || port > high) continue;
        if (std::find(ports.begin(), ports.end(), port) == ports.end())
            ports.push_back(port);
    }
    std::sort(ports.begin(), ports.end());
    return ports;
}

int port_opened_since(const std::vector<int>& before, int low, int high) {
    const auto now = listening_ports(low, high);
    for (int port : now)
        if (std::find(before.begin(), before.end(), port) == before.end()) return port;
    return 0;
}

} // namespace netinfo

