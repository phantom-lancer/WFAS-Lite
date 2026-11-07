#include "wfas/audio_output.hpp"
#include "wfas/client_session.hpp"
#include "wfas/discovery.hpp"
#include "wfas/jitter_buffer.hpp"
#include "wfas/reconnect.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace {

std::atomic<bool> stop_requested{false};

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        stop_requested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

struct Config {
    std::string ip;              // manual server address (optional)
    unsigned short port = 9090;
    unsigned latency = 120;
    long reconnect_ms = -1;      // -1 = 1/2/4/8/15 s backoff, 0 = single try
    bool discovery = false;      // explicit --discover
    bool no_discovery = false;
    bool audio = true;
    long run_seconds = -1;       // test aid
    std::string state_dir;
};

void usage() {
    std::cerr
        << "Usage: wfas-lite [IPv4] [options]\n"
        << "  With no IPv4, the client discovers a WFAS v2 server automatically\n"
        << "  (multicast 239.255.0.1:9091) and reconnects with 1/2/4/8/15 s backoff.\n"
        << "Options:\n"
        << "  --port N            streaming port for a manual IPv4 (default 9090)\n"
        << "  --latency-ms N      jitter buffer target (default 120)\n"
        << "  --discover          try discovery first, use IPv4 as fallback\n"
        << "  --no-discovery      manual IPv4 only\n"
        << "  --reconnect-ms N    fixed retry delay; 0 disables reconnecting\n"
        << "  --no-audio          do not open WASAPI (integration tests)\n"
        << "  --run-seconds N     stop after N seconds (integration tests)\n"
        << "  --state-dir PATH    where last-server.txt is stored\n"
        << "Cleartext WFAS v2 unicast: 48 kHz / 16-bit / stereo. Ctrl+C stops.\n";
}

bool parse_args(int argc, char** argv, Config& config) {
    int index = 1;
    if (index < argc && argv[index][0] != '-') {
        config.ip = argv[index++];
    }
    for (; index < argc; ++index) {
        const std::string arg = argv[index];
        auto value = [&]() -> const char* {
            return ++index < argc ? argv[index] : nullptr;
        };
        if (arg == "--port") {
            const char* text = value();
            if (!text) return false;
            config.port = static_cast<unsigned short>(std::strtoul(text, nullptr, 10));
        } else if (arg == "--latency-ms") {
            const char* text = value();
            if (!text) return false;
            config.latency = static_cast<unsigned>(std::strtoul(text, nullptr, 10));
        } else if (arg == "--reconnect-ms") {
            const char* text = value();
            if (!text) return false;
            config.reconnect_ms = std::strtol(text, nullptr, 10);
        } else if (arg == "--discover") {
            config.discovery = true;
        } else if (arg == "--no-discovery") {
            config.no_discovery = true;
        } else if (arg == "--no-audio") {
            config.audio = false;
        } else if (arg == "--run-seconds") {
            const char* text = value();
            if (!text) return false;
            config.run_seconds = std::strtol(text, nullptr, 10);
        } else if (arg == "--state-dir") {
            const char* text = value();
            if (!text) return false;
            config.state_dir = text;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            return false;
        }
    }
    if (config.discovery && config.no_discovery) return false;
    if (config.ip.empty() && config.no_discovery) return false;
    if (config.ip.empty()) config.discovery = true;
    if (config.port == 0 || config.latency < 20 || config.latency > 2000) {
        return false;
    }
    return true;
}

wfas::ServerTarget choose_target(const Config& config,
                                 wfas::DiscoveryListener* discovery,
                                 const wfas::ServerTarget& saved, bool have_saved,
                                 int wait_ms, std::ostream& out) {
    // A manual address without --discover is authoritative, even when a saved
    // server exists from a previous run.
    if (!config.discovery && !config.ip.empty()) {
        wfas::ServerTarget target;
        target.ip = config.ip;
        target.port = config.port;
        return target;
    }
    if (discovery && discovery->is_open()) {
        wfas::DiscoveryBeacon beacon;
        std::string error;
        const auto result =
            wfas::wait_for_unicast_server(*discovery, wait_ms, beacon, error);
        if (result == wfas::DiscoveryWait::found) {
            wfas::ServerTarget target;
            target.ip = beacon.source_ip;
            target.port = beacon.port;
            target.hostname = beacon.hostname;
            target.from_discovery = true;
            out << "Discovered WFAS server '" << beacon.hostname << "' at "
                << target.ip << ':' << target.port << ".\n";
            return target;
        }
        if (result == wfas::DiscoveryWait::error) {
            out << "Discovery error: " << error << ".\n";
        } else if (result == wfas::DiscoveryWait::timed_out) {
            out << "Discovery: " << error << ".\n";
        }
    }
    if (!config.ip.empty()) {
        out << "Using manual server " << config.ip << ':' << config.port << ".\n";
        wfas::ServerTarget target;
        target.ip = config.ip;
        target.port = config.port;
        return target;
    }
    if (have_saved && saved.valid()) {
        out << "Using saved server " << saved.ip << ':' << saved.port << ".\n";
        return saved;
    }
    return {};
}

void sleep_while_running(std::uint64_t milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (!stop_requested.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    if (!parse_args(argc, argv, config)) {
        usage();
        return 2;
    }
    SetConsoleCtrlHandler(console_handler, TRUE);
    // Unbuffered logs: the autostart launcher redirects stdout to a file, and a
    // background client must leave a readable trace without waiting for a full
    // buffer.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "Error: WSAStartup failed.\n";
        return 1;
    }

    const std::string state_dir = config.state_dir.empty()
                                      ? wfas::default_state_dir()
                                      : config.state_dir;
    const std::string last_server_path =
        (std::filesystem::path(state_dir) / "last-server.txt").string();

    wfas::JitterBuffer buffer(config.latency);
    wfas::StreamProcessor processor(buffer);
    std::unique_ptr<wfas::AudioSink> sink;
    if (config.audio) {
        sink = std::make_unique<wfas::WasapiSink>(buffer);
    } else {
        sink = std::make_unique<wfas::NullSink>(buffer);
    }
    std::string error;
    if (!sink->start(error)) {
        std::cerr << "Error: " << error << "\n";
        WSACleanup();
        return 1;
    }
    if (config.audio) {
        const unsigned endpoint = sink->endpoint_frames();
        std::cout << "WASAPI endpoint buffer: " << endpoint << " frames ("
                  << (endpoint / 48u) << " ms), 48 kHz stereo S16LE.\n";
    }

    wfas::UdpClientSocket client_socket;
    if (!client_socket.open(1 << 20, error)) {
        std::cerr << "Error: " << error << "\n";
        sink->stop();
        WSACleanup();
        return 1;
    }

    wfas::DiscoveryListener discovery;
    bool discovery_ready = false;
    if (!config.no_discovery) {
        if (discovery.open(error)) {
            discovery_ready = true;
        } else {
            std::cerr << "Warning: automatic discovery unavailable: " << error
                      << ". Falling back to the saved/manual server.\n";
        }
    }

    wfas::ServerTarget saved;
    bool have_saved = wfas::load_last_server(last_server_path, saved, error);
    if (have_saved) {
        std::cout << "Saved server: " << saved.ip << ':' << saved.port;
        if (!saved.hostname.empty()) std::cout << " ('" << saved.hostname << "')";
        std::cout << ".\n";
    }

    std::thread watchdog;
    if (config.run_seconds > 0) {
        const auto limit = std::chrono::seconds(config.run_seconds);
        watchdog = std::thread([limit] {
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while (!stop_requested.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            stop_requested.store(true, std::memory_order_release);
        });
    }

    wfas::ReconnectPolicy policy;
    bool first_attempt = true;
    bool fatal = false;

    while (!stop_requested.load(std::memory_order_acquire)) {
        int wait_ms = 5000;
        if (!first_attempt) {
            wait_ms = config.reconnect_ms > 0
                          ? static_cast<int>(config.reconnect_ms)
                          : static_cast<int>(policy.next_delay_ms());
        }
        const auto target = choose_target(
            config, discovery_ready ? &discovery : nullptr, saved, have_saved,
            wait_ms, std::cout);
        if (!target.valid()) {
            if (config.reconnect_ms == 0) {
                std::cerr << "Error: no WFAS server found.\n";
                break;
            }
            first_attempt = false;
            continue;
        }

        wfas::SessionConfig session;
        session.ip = target.ip;
        session.port = target.port;
        session.latency_ms = config.latency;
        session.handshake_timeout_ms = first_attempt ? 15000 : 5000;
        session.ping_timeout_ms = 3000;
        session.verbose = true;

        const auto report = wfas::run_client_session(
            session, client_socket, buffer, processor, *sink, stop_requested,
            std::cout);

        // Keep WASAPI and the socket alive; only the stale audio is dropped.
        processor.reset();
        buffer.reset();

        if (report.acknowledged) {
            if (!have_saved || saved.ip != target.ip || saved.port != target.port) {
                saved = target;
                have_saved = true;
                std::string save_error;
                if (!wfas::save_last_server(last_server_path, target, save_error)) {
                    std::cerr << "Warning: " << save_error << "\n";
                }
            }
            policy.reset();
        }

        if (report.end == wfas::SessionEnd::stopped) break;
        if (wfas::session_end_is_fatal(report.end)) {
            std::cerr << "Error: " << report.error << "\n";
            fatal = true;
            break;
        }
        if (config.reconnect_ms == 0) break;

        first_attempt = false;
    }

    if (watchdog.joinable()) watchdog.join();
    sink->stop();
    discovery.close();
    client_socket.close();
    WSACleanup();
    std::cout << "WFAS-Lite stopped cleanly.\n";
    return fatal ? 1 : 0;
}
