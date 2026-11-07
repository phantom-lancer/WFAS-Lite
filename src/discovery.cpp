#include "wfas/discovery.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace wfas {
namespace {

constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~0ull);

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && static_cast<unsigned char>(value[begin]) <= 0x20) ++begin;
    while (end > begin && (static_cast<unsigned char>(value[end - 1]) <= 0x20 ||
                           value[end - 1] == '\0')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string upper_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return value;
}

std::vector<std::string> split(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = value.find(separator, start);
        if (pos == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

bool parse_unsigned(const std::string& text, unsigned long minimum,
                    unsigned long maximum, unsigned long& out) {
    if (text.empty()) return false;
    unsigned long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned long long>(c - '0');
        if (value > maximum) return false;
    }
    if (value < minimum || value > maximum) return false;
    out = static_cast<unsigned long>(value);
    return true;
}

std::string ipv4_to_string(const in_addr& address) {
    char text[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address, text, sizeof(text))) return {};
    return text;
}

// All up, non-loopback IPv4 addresses.  Loopback is still returned so beacons
// echoed by the local machine can be filtered out.
std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> result;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(
        AF_INET, flags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(
            AF_INET, flags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return result;

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr) continue;
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* addr =
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const std::string text = ipv4_to_string(addr->sin_addr);
            if (!text.empty()) result.push_back(text);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool is_loopback(const std::string& ip) { return ip.rfind("127.", 0) == 0; }

} // namespace

bool valid_ipv4(const std::string& text) {
    in_addr address{};
    return InetPtonA(AF_INET, text.c_str(), &address) == 1;
}

DiscoveryParseResult parse_discovery_beacon(const std::string& datagram,
                                            const std::string& source_ip,
                                            DiscoveryBeacon& beacon,
                                            std::string& error) {
    error.clear();
    beacon = DiscoveryBeacon{};
    const std::string message = trim(datagram);
    const std::size_t prefix_length = std::strlen(kDiscoveryPrefix);
    if (message.size() < prefix_length ||
        message.compare(0, prefix_length, kDiscoveryPrefix) != 0) {
        return DiscoveryParseResult::not_discovery;
    }
    // The prefix must be a complete field, not a longer word that happens to
    // start with it.
    if (message.size() == prefix_length || message[prefix_length] != ';') {
        return DiscoveryParseResult::not_discovery;
    }
    if (!valid_ipv4(source_ip)) {
        error = "beacon source address is not IPv4: " + source_ip;
        return DiscoveryParseResult::malformed;
    }

    const auto parts = split(message, ';');
    if (parts.size() < 4) {
        error = "beacon has fewer than four fields";
        return DiscoveryParseResult::malformed;
    }

    beacon.source_ip = source_ip;
    beacon.raw = message;
    beacon.hostname = trim(parts[1]);
    if (beacon.hostname.empty()) {
        error = "beacon hostname is empty";
        return DiscoveryParseResult::malformed;
    }
    if (beacon.hostname.size() > 255) {
        error = "beacon hostname is too long";
        return DiscoveryParseResult::malformed;
    }

    const std::string mode = upper_ascii(trim(parts[2]));
    if (mode == "BYE") {
        beacon.mode = DiscoveryMode::bye;
        beacon.port = 0;
        return DiscoveryParseResult::bye;
    }
    if (mode == "UNICAST") {
        beacon.mode = DiscoveryMode::unicast;
    } else if (mode == "MULTICAST") {
        beacon.mode = DiscoveryMode::multicast;
    } else {
        error = "unknown discovery mode: " + parts[2];
        return DiscoveryParseResult::malformed;
    }

    unsigned long port = 0;
    if (!parse_unsigned(trim(parts[3]), 1, 65535, port)) {
        error = "invalid streaming port: " + parts[3];
        return DiscoveryParseResult::malformed;
    }
    beacon.port = static_cast<unsigned short>(port);

    for (std::size_t i = 4; i < parts.size(); ++i) {
        const std::string& token = parts[i];
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) continue;  // tolerate bare tokens
        const std::string key = upper_ascii(trim(token.substr(0, equals)));
        const std::string value = trim(token.substr(equals + 1));
        unsigned long number = 0;

        if (key == "PROTOCOLS") {
            bool wfas = false;
            for (const auto& protocol : split(value, ',')) {
                if (upper_ascii(trim(protocol)) == "WFAS") wfas = true;
            }
            beacon.speaks_wfas = wfas;
        } else if (key == "HTTP_PORT") {
            if (!parse_unsigned(value, 1, 65535, number)) {
                error = "invalid http_port: " + value;
                return DiscoveryParseResult::malformed;
            }
        } else if (key == "SR") {
            if (!parse_unsigned(value, 8000, 192000, number)) {
                error = "invalid sample rate: " + value;
                return DiscoveryParseResult::malformed;
            }
            beacon.sample_rate = static_cast<unsigned>(number);
        } else if (key == "CH") {
            if (!parse_unsigned(value, 1, 8, number)) {
                error = "invalid channel count: " + value;
                return DiscoveryParseResult::malformed;
            }
            beacon.channels = static_cast<unsigned>(number);
        } else if (key == "BD") {
            if (!parse_unsigned(value, 8, 32, number) ||
                (number != 8 && number != 16 && number != 24 && number != 32)) {
                error = "invalid bit depth: " + value;
                return DiscoveryParseResult::malformed;
            }
            beacon.bit_depth = static_cast<unsigned>(number);
        } else if (key == "AUTH") {
            const std::string auth = upper_ascii(value);
            if (auth == "OFF") {
                beacon.requires_key = false;
            } else if (auth == "ASK" || auth == "KEY") {
                beacon.requires_key = true;
            } else {
                error = "invalid auth mode: " + value;
                return DiscoveryParseResult::malformed;
            }
        } else if (key == "ENC") {
            if (value == "0") {
                beacon.encrypted = false;
            } else if (value == "1") {
                beacon.encrypted = true;
                beacon.requires_key = true;
            } else {
                error = "invalid enc flag: " + value;
                return DiscoveryParseResult::malformed;
            }
        } else if (key == "MIC") {
            const std::string mic = upper_ascii(value);
            if (mic != "TX" && mic != "RX" && mic != "TXRX") {
                error = "invalid mic hint: " + value;
                return DiscoveryParseResult::malformed;
            }
        }
        // Unknown tokens are ignored by design (forward compatibility).
    }
    return DiscoveryParseResult::ok;
}

bool usable_unicast_beacon(const DiscoveryBeacon& beacon, std::string* reason) {
    auto fail = [&](const char* text) {
        if (reason) *reason = text;
        return false;
    };
    if (beacon.mode != DiscoveryMode::unicast) return fail("not a UNICAST server");
    if (beacon.port == 0) return fail("invalid streaming port");
    if (!beacon.speaks_wfas) return fail("server does not advertise WFAS");
    if (beacon.requires_key || beacon.encrypted) {
        return fail("server requires a key/encryption (unsupported)");
    }
    if (!valid_ipv4(beacon.source_ip)) return fail("invalid source address");
    if (reason) reason->clear();
    return true;
}

DiscoveryListener::DiscoveryListener() : socket_(kInvalidSocket) {}

DiscoveryListener::~DiscoveryListener() { close(); }

bool DiscoveryListener::open(std::string& error) {
    error.clear();
    if (open_) return true;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        error = "cannot create discovery UDP socket";
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    int receive_buffer = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receive_buffer),
               sizeof(receive_buffer));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kDiscoveryPort);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR) {
        error = "cannot bind UDP port 9091 for discovery (already in use?)";
        closesocket(sock);
        return false;
    }

    local_ips_ = local_ipv4_addresses();

    int joined = 0;
    for (const auto& ip : local_ips_) {
        if (is_loopback(ip)) continue;
        ip_mreq request{};
        if (InetPtonA(AF_INET, kDiscoveryGroup, &request.imr_multiaddr) != 1) continue;
        if (InetPtonA(AF_INET, ip.c_str(), &request.imr_interface) != 1) continue;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&request),
                       sizeof(request)) == 0) {
            ++joined;
        }
    }
    if (joined == 0) {
        // No usable unicast interface found (or all joins failed): let the
        // routing table choose the multicast interface.
        ip_mreq request{};
        InetPtonA(AF_INET, kDiscoveryGroup, &request.imr_multiaddr);
        request.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&request), sizeof(request));
    }

    socket_ = static_cast<std::uintptr_t>(sock);
    open_ = true;
    return true;
}

void DiscoveryListener::close() {
    if (!open_) return;
    const auto sock = static_cast<SOCKET>(socket_);
    closesocket(sock);
    socket_ = kInvalidSocket;
    open_ = false;
    local_ips_.clear();
}

bool DiscoveryListener::is_local(const std::string& ip) const {
    return std::find(local_ips_.begin(), local_ips_.end(), ip) != local_ips_.end();
}

void DiscoveryListener::inject_for_testing(const std::string& datagram,
                                           const std::string& source_ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    injected_.emplace_back(datagram, source_ip);
}

bool DiscoveryListener::receive(DiscoveryBeacon& beacon, int timeout_ms,
                                std::string& error) {
    error.clear();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!injected_.empty()) {
            auto item = injected_.front();
            injected_.pop_front();
            DiscoveryBeacon parsed;
            std::string parse_error;
            const auto result =
                parse_discovery_beacon(item.first, item.second, parsed, parse_error);
            if (result == DiscoveryParseResult::ok) {
                beacon = parsed;
                return true;
            }
            ++rejected_;
        }
    }

    if (!open_) {
        error = "discovery socket is not open";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    const auto sock = static_cast<SOCKET>(socket_);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = "timeout";
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        timeval tv{};
        tv.tv_sec = static_cast<long>(remaining.count() / 1000);
        tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
        const int selected = select(0, &read_set, nullptr, nullptr, &tv);
        if (selected == SOCKET_ERROR) {
            error = "discovery select failed";
            return false;
        }
        if (selected == 0) {
            error = "timeout";
            return false;
        }

        char buffer[2048]{};
        sockaddr_in from{};
        int from_length = sizeof(from);
        const int bytes = recvfrom(sock, buffer, sizeof(buffer), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_length);
        if (bytes == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) continue;
            error = "discovery receive failed";
            return false;
        }
        if (bytes <= 0) continue;

        const std::string source = ipv4_to_string(from.sin_addr);
        if (source.empty()) {
            ++rejected_;
            continue;
        }
        // A server running on this same machine is a valid target (useful for
        // local testing), so beacons are not filtered by source address.  The
        // client never announces, so it cannot discover itself.
        DiscoveryBeacon parsed;
        std::string parse_error;
        const std::string datagram(buffer, buffer + bytes);
        const auto result =
            parse_discovery_beacon(datagram, source, parsed, parse_error);
        if (result == DiscoveryParseResult::ok) {
            beacon = parsed;
            return true;
        }
        ++rejected_;
    }
}

DiscoveryWait wait_for_unicast_server(DiscoveryListener& listener, int timeout_ms,
                                      DiscoveryBeacon& beacon,
                                      std::string& error) {
    error.clear();
    if (!listener.is_open()) {
        error = "discovery socket is not open";
        return DiscoveryWait::closed;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    std::string last_reason = "no discovery beacon received";
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = last_reason;
            return DiscoveryWait::timed_out;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        DiscoveryBeacon candidate;
        std::string receive_error;
        if (!listener.receive(candidate, static_cast<int>(remaining.count()) + 1,
                              receive_error)) {
            if (receive_error == "timeout") {
                error = last_reason;
                return DiscoveryWait::timed_out;
            }
            error = receive_error;
            return DiscoveryWait::error;
        }
        std::string reason;
        if (usable_unicast_beacon(candidate, &reason)) {
            beacon = candidate;
            return DiscoveryWait::found;
        }
        last_reason = candidate.hostname + ": " + reason;
    }
}

} // namespace wfas
