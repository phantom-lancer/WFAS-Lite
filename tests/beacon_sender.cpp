// Test/diagnostic tool: a WFAS v2 unicast server that also emits the standard
// discovery beacon on 239.255.0.1:9091.  It stops announcing while a client is
// connected (like the upstream server) and resumes after CLIENT_BYE/BYE, which
// is exactly the behaviour the client's reconnect path relies on.
#include "wfas/discovery.hpp"
#include "wfas/protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kPacketFrames = 480;

struct Options {
    unsigned short port = 19090;
    unsigned short discovery_port = 9091;
    int seconds = 30;
    int session_seconds = 3;
    bool announce = true;
    bool announce_while_busy = false;
};

std::string beacon_message(unsigned short port) {
    return std::string(wfas::kDiscoveryPrefix) +
           ";beacon-test;UNICAST;" + std::to_string(port) +
           ";protocols=WFAS;sr=48000;ch=2;bd=16;auth=OFF;enc=0";
}

void send_beacon(SOCKET socket, const sockaddr_in& group,
                 const std::string& message) {
    sendto(socket, message.data(), static_cast<int>(message.size()), 0,
           reinterpret_cast<const sockaddr*>(&group), sizeof(group));
}
} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char* { return ++i < argc ? argv[i] : nullptr; };
        if (arg == "--port") {
            const char* v = value();
            if (v) options.port = static_cast<unsigned short>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--seconds") {
            const char* v = value();
            if (v) options.seconds = std::atoi(v);
        } else if (arg == "--session-seconds") {
            const char* v = value();
            if (v) options.session_seconds = std::atoi(v);
        } else if (arg == "--announce-while-busy") {
            options.announce_while_busy = true;
        }
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    // 1 ms timer resolution keeps the synthetic sender close to real time.
    timeBeginPeriod(1);

    SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET beacon = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server == INVALID_SOCKET || beacon == INVALID_SOCKET) return 1;

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(options.port);
    if (bind(server, reinterpret_cast<sockaddr*>(&local), sizeof(local)) ==
        SOCKET_ERROR) {
        std::cerr << "bind failed on port " << options.port << "\n";
        return 1;
    }
    u_long non_blocking = 1;
    ioctlsocket(server, FIONBIO, &non_blocking);

    int ttl = 4;
    setsockopt(beacon, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(options.discovery_port);
    InetPtonA(AF_INET, wfas::kDiscoveryGroup, &group.sin_addr);

    std::cout << "Beacon sender: WFAS server on UDP " << options.port
              << ", discovery " << wfas::kDiscoveryGroup << ':'
              << options.discovery_port << ", session "
              << options.session_seconds << "s, run " << options.seconds
              << "s\n";

    sockaddr_in client{};
    bool session = false;
    bool have_client = false;
    auto session_start = Clock::now();
    auto next_audio = Clock::now();
    auto next_ping = Clock::now();
    auto next_beacon = Clock::now();
    std::uint16_t sequence = 0;
    std::uint32_t position = 0;
    double phase = 0.0;
    std::vector<std::uint8_t> pcm(kPacketFrames * wfas::kStereoFrameBytes);
    const auto start = Clock::now();
    char buffer[2048];

    while (Clock::now() - start < std::chrono::seconds(options.seconds)) {
        const auto now = Clock::now();
        for (;;) {
            sockaddr_in from{};
            int from_length = sizeof(from);
            const int bytes = recvfrom(server, buffer, sizeof(buffer), 0,
                                       reinterpret_cast<sockaddr*>(&from),
                                       &from_length);
            if (bytes == SOCKET_ERROR) break;
            if (bytes <= 0) continue;
            const std::string message(buffer, buffer + bytes);
            if (message == "CLIENT_BYE") {
                if (session) {
                    std::cout << "CLIENT_BYE from client; session released\n";
                    session = false;
                    next_beacon = now;
                }
                continue;
            }
            if (message.rfind("HELLO_FROM_CLIENT", 0) == 0) {
                if (session) {
                    sendto(server, "WFAS_BUSY", 9, 0,
                           reinterpret_cast<const sockaddr*>(&from), from_length);
                    continue;
                }
                client = from;
                have_client = true;
                session = true;
                session_start = now;
                next_audio = now;
                next_ping = now;
                sequence = 0;
                position = 0;
                phase = 0.0;
                sendto(server, "HELLO_ACK;v=2", 13, 0,
                       reinterpret_cast<const sockaddr*>(&client), sizeof(client));
                std::cout << "HELLO -> HELLO_ACK\n";
            }
        }

        if (session &&
            now - session_start >= std::chrono::seconds(options.session_seconds)) {
            sendto(server, "BYE", 3, 0,
                   reinterpret_cast<const sockaddr*>(&client), sizeof(client));
            std::cout << "Session ended (BYE)\n";
            session = false;
            next_beacon = now;
        }

        if (session) {
            // Send in a catch-up loop: Windows' default timer granularity is
            // ~15.6 ms, so a single "if" per wake-up would under-run real time.
            int catch_up = 0;
            while (now >= next_audio && catch_up < 8) {
                for (std::size_t i = 0; i < kPacketFrames; ++i) {
                    const auto value = static_cast<std::int16_t>(
                        std::sin(phase) * 7000.0);
                    phase += 2.0 * 3.141592653589793 * 440.0 / kRate;
                    pcm[i * 4] = static_cast<std::uint8_t>(value);
                    pcm[i * 4 + 1] = static_cast<std::uint8_t>(value >> 8);
                    pcm[i * 4 + 2] = pcm[i * 4];
                    pcm[i * 4 + 3] = pcm[i * 4 + 1];
                }
                const auto packet = wfas::make_audio_packet(
                    sequence++, position, false, pcm.data(), pcm.size());
                sendto(server, reinterpret_cast<const char*>(packet.data()),
                       static_cast<int>(packet.size()), 0,
                       reinterpret_cast<const sockaddr*>(&client), sizeof(client));
                position += static_cast<std::uint32_t>(kPacketFrames);
                next_audio += std::chrono::milliseconds(10);
                ++catch_up;
            }
            if (next_audio < now - std::chrono::milliseconds(100)) {
                next_audio = now;
            }
            if (now >= next_ping) {
                sendto(server, "PING", 4, 0,
                       reinterpret_cast<const sockaddr*>(&client), sizeof(client));
                next_ping = now + std::chrono::seconds(1);
            }
        }

        const bool announce = options.announce &&
                              (!session || options.announce_while_busy);
        if (announce && now >= next_beacon) {
            send_beacon(beacon, group, beacon_message(options.port));
            next_beacon = now + std::chrono::milliseconds(300);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (session && have_client) {
        sendto(server, "BYE", 3, 0, reinterpret_cast<const sockaddr*>(&client),
               sizeof(client));
    }
    closesocket(server);
    closesocket(beacon);
    WSACleanup();
    timeEndPeriod(1);
    std::cout << "Beacon sender stopped\n";
    return 0;
}
