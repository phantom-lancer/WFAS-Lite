// Discovery beacon parsing, unicast selection, the multicast listener and the
// reconnect/fallback policy.  No network access is required: the listener is
// driven through its injection hook.
#include "wfas/discovery.hpp"
#include "wfas/reconnect.hpp"

#include <winsock2.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool value, const std::string& name) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << name << "\n";
    }
}

constexpr const char* kIp = "192.168.1.125";

std::string beacon(const std::string& host, const std::string& mode,
                   const std::string& port, const std::string& extra = "") {
    return std::string(wfas::kDiscoveryPrefix) + ";" + host + ";" + mode + ";" +
           port + (extra.empty() ? "" : ";" + extra);
}

void test_valid_unicast_beacon() {
    const std::string message = beacon(
        "studio-pc", "UNICAST", "9090",
        "protocols=WFAS;http_port=8080;sr=48000;ch=2;bd=16;auth=OFF;enc=0;mic=tx");
    wfas::DiscoveryBeacon parsed;
    std::string error;
    check(wfas::parse_discovery_beacon(message, kIp, parsed, error) ==
              wfas::DiscoveryParseResult::ok,
          "valid: parsed");
    check(parsed.hostname == "studio-pc", "valid: hostname");
    check(parsed.source_ip == kIp, "valid: source ip comes from the packet");
    check(parsed.mode == wfas::DiscoveryMode::unicast, "valid: UNICAST mode");
    check(parsed.port == 9090, "valid: port");
    check(parsed.speaks_wfas, "valid: WFAS protocol");
    check(!parsed.requires_key, "valid: cleartext");
    check(!parsed.encrypted, "valid: not encrypted");
    check(parsed.sample_rate == 48000 && parsed.channels == 2 &&
              parsed.bit_depth == 16,
          "valid: audio fields");
    check(wfas::usable_unicast_beacon(parsed), "valid: usable for connection");
}

void test_multicast_and_key_are_rejected() {
    wfas::DiscoveryBeacon parsed;
    std::string error;
    const std::string multicast = beacon("living-room", "MULTICAST", "9090");
    check(wfas::parse_discovery_beacon(multicast, kIp, parsed, error) ==
              wfas::DiscoveryParseResult::ok,
          "multicast: parses");
    check(!wfas::usable_unicast_beacon(parsed), "multicast: not usable");

    const std::string key = beacon("secure-pc", "UNICAST", "9090", "auth=KEY;enc=1");
    check(wfas::parse_discovery_beacon(key, kIp, parsed, error) ==
              wfas::DiscoveryParseResult::ok,
          "key: parses");
    check(parsed.requires_key, "key: marked as requiring a key");
    check(!wfas::usable_unicast_beacon(parsed), "key: not usable");

    const std::string rtp = beacon("rtp-pc", "UNICAST", "9090", "protocols=RTP,HTTP");
    check(wfas::parse_discovery_beacon(rtp, kIp, parsed, error) ==
              wfas::DiscoveryParseResult::ok,
          "rtp: parses");
    check(!parsed.speaks_wfas, "rtp: WFAS absent");
    check(!wfas::usable_unicast_beacon(parsed), "rtp: not usable");
}

void test_invalid_beacons() {
    struct Case {
        std::string datagram;
        std::string source;
        wfas::DiscoveryParseResult expected;
        const char* name;
    };
    const std::string prefix = wfas::kDiscoveryPrefix;
    const std::vector<Case> cases = {
        {"", kIp, wfas::DiscoveryParseResult::not_discovery, "empty"},
        {"SOME_OTHER_PROTOCOL", kIp, wfas::DiscoveryParseResult::not_discovery,
         "other protocol"},
        {prefix, kIp, wfas::DiscoveryParseResult::not_discovery,
         "prefix without fields"},
        {prefix + "X;host;UNICAST;9090", kIp,
         wfas::DiscoveryParseResult::not_discovery, "prefix is not a field"},
        {beacon("host", "UNICAST", "9090"), "not-an-ip",
         wfas::DiscoveryParseResult::malformed, "invalid source address"},
        {prefix + ";host;UNICAST", kIp, wfas::DiscoveryParseResult::malformed,
         "too few fields"},
        {beacon("", "UNICAST", "9090"), kIp,
         wfas::DiscoveryParseResult::malformed, "empty hostname"},
        {beacon("host", "SIDECAR", "9090"), kIp,
         wfas::DiscoveryParseResult::malformed, "unknown mode"},
        {beacon("host", "UNICAST", "0"), kIp,
         wfas::DiscoveryParseResult::malformed, "port zero"},
        {beacon("host", "UNICAST", "65536"), kIp,
         wfas::DiscoveryParseResult::malformed, "port too large"},
        {beacon("host", "UNICAST", "abc"), kIp,
         wfas::DiscoveryParseResult::malformed, "port not numeric"},
        {beacon("host", "UNICAST", "9090", "sr=abc"), kIp,
         wfas::DiscoveryParseResult::malformed, "bad sample rate"},
        {beacon("host", "UNICAST", "9090", "ch=0"), kIp,
         wfas::DiscoveryParseResult::malformed, "bad channel count"},
        {beacon("host", "UNICAST", "9090", "bd=12"), kIp,
         wfas::DiscoveryParseResult::malformed, "bad bit depth"},
        {beacon("host", "UNICAST", "9090", "auth=MAYBE"), kIp,
         wfas::DiscoveryParseResult::malformed, "bad auth mode"},
        {beacon("host", "UNICAST", "9090", "enc=2"), kIp,
         wfas::DiscoveryParseResult::malformed, "bad enc flag"},
        {beacon("host", "UNICAST", "9090", "mic=both"), kIp,
         wfas::DiscoveryParseResult::malformed, "bad mic hint"},
        {beacon("host", "UNICAST", "9090", "http_port=0"), kIp,
         wfas::DiscoveryParseResult::malformed, "bad http port"},
    };
    for (const auto& item : cases) {
        wfas::DiscoveryBeacon parsed;
        std::string error;
        const auto result =
            wfas::parse_discovery_beacon(item.datagram, item.source, parsed, error);
        check(result == item.expected,
              std::string("invalid: ") + item.name + " -> " +
                  (result == item.expected ? "ok" : "wrong result"));
        if (item.expected == wfas::DiscoveryParseResult::malformed) {
            check(!error.empty(), std::string("invalid: ") + item.name +
                                      " has an error message");
        }
    }
}

void test_bye_and_unknown_tokens() {
    wfas::DiscoveryBeacon parsed;
    std::string error;
    check(wfas::parse_discovery_beacon(
              std::string(wfas::kDiscoveryPrefix) + ";host;BYE;0", kIp, parsed,
              error) == wfas::DiscoveryParseResult::bye,
          "bye: recognised");
    check(parsed.mode == wfas::DiscoveryMode::bye && parsed.port == 0,
          "bye: mode and port");

    const std::string message = beacon("host", "unicast", "9090",
                                       "future=1;protocols=WFAS;auth=off");
    check(wfas::parse_discovery_beacon(message, kIp, parsed, error) ==
              wfas::DiscoveryParseResult::ok,
          "unknown tokens: ignored");
    check(parsed.port == 9090 && !parsed.requires_key,
          "unknown tokens: fields preserved");

    const std::string crlf = beacon("host", "UNICAST", "9090") + "\r\n";
    check(wfas::parse_discovery_beacon(crlf, kIp, parsed, error) ==
              wfas::DiscoveryParseResult::ok,
          "trailing CRLF: tolerated");
}

void test_wait_for_unicast_server() {
    wfas::DiscoveryListener listener;
    std::string error;
    if (!listener.open(error)) {
        check(false, "listener: opens on 239.255.0.1:9091 (" + error + ")");
        return;
    }
    listener.inject_for_testing("garbage", "192.168.1.9");
    listener.inject_for_testing(beacon("tv", "MULTICAST", "9090"), "192.168.1.10");
    listener.inject_for_testing(beacon("secure", "UNICAST", "9090", "auth=KEY"),
                                "192.168.1.11");
    listener.inject_for_testing(beacon("desk", "UNICAST", "9095"), "192.168.1.12");

    wfas::DiscoveryBeacon found;
    const auto result = wfas::wait_for_unicast_server(listener, 500, found, error);
    check(result == wfas::DiscoveryWait::found, "wait: found the unicast server");
    check(found.hostname == "desk" && found.port == 9095,
          "wait: correct server selected");
    check(found.source_ip == "192.168.1.12", "wait: source ip preserved");
    check(listener.rejected() >= 1, "wait: invalid beacons counted");

    wfas::DiscoveryBeacon none;
    const auto timeout = wfas::wait_for_unicast_server(listener, 50, none, error);
    check(timeout == wfas::DiscoveryWait::timed_out, "wait: timeout with no data");
    listener.close();
    check(!listener.is_open(), "listener: closed");
}

void test_listener_reopen_and_no_leak() {
    // Opening and closing repeatedly must not leak sockets or fail on reuse.
    for (int i = 0; i < 10; ++i) {
        wfas::DiscoveryListener listener;
        std::string error;
        check(listener.open(error),
              "listener: reopen " + std::to_string(i) + " (" + error + ")");
        listener.close();
    }
}

void test_reconnect_backoff() {
    wfas::ReconnectPolicy policy;
    check(policy.next_delay_ms() == 1000, "backoff: 1 s");
    check(policy.next_delay_ms() == 2000, "backoff: 2 s");
    check(policy.next_delay_ms() == 4000, "backoff: 4 s");
    check(policy.next_delay_ms() == 8000, "backoff: 8 s");
    check(policy.next_delay_ms() == 15000, "backoff: 15 s");
    check(policy.next_delay_ms() == 15000, "backoff: stays at 15 s");
    policy.reset();
    check(policy.next_delay_ms() == 1000, "backoff: reset after success");
}

void test_saved_server_round_trip() {
    const auto directory =
        std::filesystem::temp_directory_path() / "wfas-lite-discovery-tests";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const auto path = (directory / "last-server.txt").string();
    std::filesystem::remove(path, ignored);

    wfas::ServerTarget saved;
    std::string error;
    check(!wfas::load_last_server(path, saved, error),
          "saved: missing file is not an error state");

    wfas::ServerTarget target;
    target.ip = "192.168.1.125";
    target.port = 9090;
    target.hostname = "studio-pc";
    check(wfas::save_last_server(path, target, error),
          "saved: write succeeds (" + error + ")");

    wfas::ServerTarget loaded;
    check(wfas::load_last_server(path, loaded, error), "saved: read succeeds");
    check(loaded.ip == target.ip && loaded.port == target.port &&
              loaded.hostname == target.hostname,
          "saved: values round-trip");

    {
        std::ofstream broken(path, std::ios::trunc);
        broken << "ip=not-an-ip\nport=0\n";
    }
    check(!wfas::load_last_server(path, loaded, error),
          "saved: malformed file rejected");

    wfas::ServerTarget invalid;
    invalid.ip = "not-an-ip";
    invalid.port = 9090;
    check(!wfas::save_last_server(path, invalid, error),
          "saved: invalid target rejected");

    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    test_valid_unicast_beacon();
    test_multicast_and_key_are_rejected();
    test_invalid_beacons();
    test_bye_and_unknown_tokens();
    test_wait_for_unicast_server();
    test_listener_reopen_and_no_leak();
    test_reconnect_backoff();
    test_saved_server_round_trip();
    WSACleanup();

    if (failures) {
        std::cerr << failures << " discovery test(s) failed.\n";
        return 1;
    }
    std::cout << "All discovery and reconnect policy tests passed.\n";
    return 0;
}
