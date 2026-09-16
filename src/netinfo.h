// Address information for display: what IP a source resolves to, and what
// address other machines would use to reach this one.
#pragma once
#include <string>
#include <vector>

namespace netinfo {

// Must be called once before anything else here. Safe to call repeatedly.
bool init();
void shutdown();

// First usable non loopback IPv4 of an up, non virtual adapter. Empty when
// there is no usable network.
std::string local_ipv4();

// Resolves a host name to an address for display. Returns an empty string if
// it does not resolve. Blocking, so call it off the UI thread.
std::string resolve(const std::string& host);

// True when something accepts a TCP connection on host:port within the
// timeout. Uses a non blocking socket, so an unreachable host cannot park the
// caller on the system's own connect timeout.
bool port_open(const std::string& host, const std::string& port, int timeout_ms);

// TCP ports this process is listening on within [low, high]. libomt does not
// report which port a sender bound to, so it is read back from the system.
std::vector<int> listening_ports(int low, int high);

// The port a sender took, found by diffing what this process was listening on
// before and after it was created. Exact, where picking the lowest or highest
// is only a guess once more than one sender is running. 0 if it cannot be told.
int port_opened_since(const std::vector<int>& before, int low, int high);

} // namespace netinfo
