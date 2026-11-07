#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wfas {

// WFAS v2 discovery (WFAS_PROTOCOL.md, section 3).  The server multicasts a
// ';'-separated ASCII beacon every ~3 s.  The client never scans the LAN: it
// only listens on this one group and port.
constexpr const char* kDiscoveryGroup = "239.255.0.1";
constexpr unsigned short kDiscoveryPort = 9091;
constexpr const char* kDiscoveryPrefix = "WIFI_AUDIO_STREAMER_DISCOVERY";

enum class DiscoveryMode { unicast, multicast, bye };

struct DiscoveryBeacon {
    std::string hostname;
    std::string source_ip;  // taken from the UDP source address, not the text
    DiscoveryMode mode{DiscoveryMode::unicast};
    unsigned short port{};
    bool speaks_wfas{true};
    bool requires_key{false};
    bool encrypted{false};
    unsigned sample_rate{};
    unsigned channels{};
    unsigned bit_depth{};
    std::string raw;  // original datagram, for diagnostics
};

enum class DiscoveryParseResult { ok, not_discovery, bye, malformed };

// Parses one beacon.  `source_ip` is the UDP source address.  Every field that
// is present must be valid; unknown key=value tokens are ignored, as required
// by the protocol.
DiscoveryParseResult parse_discovery_beacon(const std::string& datagram,
                                            const std::string& source_ip,
                                            DiscoveryBeacon& beacon,
                                            std::string& error);

// True when a unicast, cleartext, WFAS-capable server can be used by this
// client.  `reason` receives a human readable explanation when false.
bool usable_unicast_beacon(const DiscoveryBeacon& beacon,
                           std::string* reason = nullptr);

bool valid_ipv4(const std::string& text);

// Multicast listener for 239.255.0.1:9091.  Joins the group on every up IPv4
// interface so it works with Wi-Fi and Ethernet without configuration.
class DiscoveryListener {
public:
    DiscoveryListener();
    ~DiscoveryListener();
    DiscoveryListener(const DiscoveryListener&) = delete;
    DiscoveryListener& operator=(const DiscoveryListener&) = delete;

    bool open(std::string& error);
    void close();
    bool is_open() const { return open_; }

    // Returns true for one syntactically valid beacon (unicast or multicast).
    // Malformed, self-originated and BYE datagrams are skipped and counted in
    // rejected().  Returns false on timeout or socket error.
    bool receive(DiscoveryBeacon& beacon, int timeout_ms, std::string& error);

    unsigned rejected() const { return rejected_; }
    const std::vector<std::string>& local_addresses() const { return local_ips_; }

    // Test hook: feed a datagram without touching the network.  Consumed before
    // any real socket data.
    void inject_for_testing(const std::string& datagram,
                            const std::string& source_ip);

private:
    bool is_local(const std::string& ip) const;

    std::uintptr_t socket_{};
    bool open_{};
    std::vector<std::string> local_ips_;
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> injected_;
    unsigned rejected_{};
};

enum class DiscoveryWait { found, timed_out, closed, error };

// Waits until a usable unicast server beacon arrives or the timeout expires.
DiscoveryWait wait_for_unicast_server(DiscoveryListener& listener, int timeout_ms,
                                      DiscoveryBeacon& beacon,
                                      std::string& error);

} // namespace wfas
