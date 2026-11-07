#include "wfas/protocol.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
int main(int argc, char** argv) {
    const unsigned short port = argc > 1 ? static_cast<unsigned short>(std::strtoul(argv[1], nullptr, 10)) : 9090;
    const unsigned packet_limit = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 0;
    WSADATA wd{}; if (WSAStartup(MAKEWORD(2,2), &wd) != 0) return 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET) return 1;
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(port); local.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) { std::cerr << "bind failed\n"; return 1; }
    std::cout << "Fake sender listening on UDP " << port << "; connect wfas-lite to 127.0.0.1. Ctrl+C stops it.\n";
    sockaddr_in client{}; int client_len = sizeof(client); char hello[256]{};
    for (;;) {
        const int n = recvfrom(s, hello, sizeof(hello)-1, 0, reinterpret_cast<sockaddr*>(&client), &client_len);
        if (n > 0 && std::string(hello, hello + n) == wfas::hello_message()) break;
    }
    const char ack[] = "HELLO_ACK;v=2";
    sendto(s, ack, sizeof(ack)-1, 0, reinterpret_cast<sockaddr*>(&client), client_len);
    constexpr unsigned sample_rate = 48000, frames_per_packet = 480;
    std::vector<std::uint8_t> pcm(frames_per_packet * wfas::kStereoFrameBytes);
    std::uint16_t sequence = 0; std::uint32_t position = 0; double phase = 0;
    auto next_packet = std::chrono::steady_clock::now();
    for (unsigned packet_count = 0;; ++packet_count) {
        for (unsigned f = 0; f < frames_per_packet; ++f) {
            const auto v = static_cast<std::int16_t>(std::sin(phase) * 7000.0); phase += 2.0 * 3.141592653589793 * 440.0 / sample_rate;
            pcm[f*4] = static_cast<std::uint8_t>(v); pcm[f*4+1] = static_cast<std::uint8_t>(v >> 8);
            pcm[f*4+2] = pcm[f*4]; pcm[f*4+3] = pcm[f*4+1];
        }
        // Deliberately skip one 10-ms packet once to exercise client loss handling.
        if (packet_count != 200) {
            auto packet = wfas::make_audio_packet(sequence, position, false, pcm.data(), pcm.size());
            sendto(s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                   reinterpret_cast<sockaddr*>(&client), client_len);
        }
        ++sequence; position += frames_per_packet;
        if (packet_count % 100 == 0) sendto(s, "PING", 4, 0, reinterpret_cast<sockaddr*>(&client), client_len);
        if (packet_limit && packet_count + 1 >= packet_limit) {
            sendto(s, "BYE", 3, 0, reinterpret_cast<sockaddr*>(&client), client_len);
            std::cout << "Fake sender sent " << (packet_count + 1) << " packets and BYE.\n";
            closesocket(s); WSACleanup(); return 0;
        }
        // Absolute schedule: sleep_for(10 ms) is ~15.6 ms on Windows without a
        // high-resolution timer, which would make the fake stream slower than
        // real time.  Catch-up sends keep the average at 10 ms per packet.
        next_packet += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next_packet);
    }
}
