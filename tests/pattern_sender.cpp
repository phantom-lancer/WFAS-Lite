// Diagnostic sender used only by the Stage A.2 investigation.  It emulates
// the default Linux WFAS 1.2.0 native sender:
//   * one 480-frame capture block per 10 ms, split into 347 + 133 frames
//     because of maxPayloadBytes = 1390;
//   * one deliberately reordered pair (the 133-frame tail overtakes the
//     347-frame head);
//   * one deliberate 300 ms sender stall (no packets, no sequence advance,
//     which is what a capture/engine hiccup looks like to the client).
// It is not part of the application and does not change the protocol.
#include "wfas/protocol.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {
constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kHead = 347;
constexpr std::size_t kTail = 133;
constexpr std::size_t kFrameBytes = wfas::kStereoFrameBytes;

std::vector<std::uint8_t> tone(std::uint32_t start_frame, std::size_t frames) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(start_frame + i) / kRate;
        const auto value = static_cast<std::int16_t>(7000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * t));
        std::memcpy(&out[i * kFrameBytes], &value, 2);
        std::memcpy(&out[i * kFrameBytes + 2], &value, 2);
    }
    return out;
}

void send_audio(SOCKET socket, const sockaddr_in& peer, std::uint16_t sequence,
                std::uint32_t position, std::size_t frames) {
    const auto pcm = tone(position, frames);
    const auto packet = wfas::make_audio_packet(sequence, position, false, pcm.data(), pcm.size());
    sendto(socket, reinterpret_cast<const char*>(packet.data()),
           static_cast<int>(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
}
} // namespace

int main(int argc, char** argv) {
    const unsigned short port =
        argc > 1 ? static_cast<unsigned short>(std::strtoul(argv[1], nullptr, 10)) : 19091;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return 1;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        std::cerr << "bind failed\n";
        return 1;
    }
    std::cout << "Diagnostic pair sender on UDP " << port << "\n";

    sockaddr_in client{};
    int client_len = sizeof(client);
    char hello[256]{};
    for (;;) {
        const int n = recvfrom(socket, hello, sizeof(hello) - 1, 0,
                               reinterpret_cast<sockaddr*>(&client), &client_len);
        if (n > 0 && std::string(hello, hello + n) == wfas::hello_message()) break;
    }
    const char ack[] = "HELLO_ACK;v=2";
    sendto(socket, ack, sizeof(ack) - 1, 0,
           reinterpret_cast<sockaddr*>(&client), client_len);

    std::uint16_t sequence = 0;
    std::uint32_t position = 0;
    auto next_tick = std::chrono::steady_clock::now();
    constexpr int kTicks = 550; // 5.5 s
    for (int tick = 0; tick < kTicks; ++tick) {
        // 300 ms of no packets: a sender-side stall, not packet loss.
        if (tick >= 250 && tick < 280) {
            next_tick += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next_tick);
            continue;
        }
        if (tick == 120) {
            // Deliberate reorder: tail first, head second.  Both carry the
            // sequence numbers they would have had in normal order.
            send_audio(socket, client, static_cast<std::uint16_t>(sequence + 1),
                       position + static_cast<std::uint32_t>(kHead), kTail);
            send_audio(socket, client, sequence, position, kHead);
        } else {
            send_audio(socket, client, sequence, position, kHead);
            send_audio(socket, client, static_cast<std::uint16_t>(sequence + 1),
                       position + static_cast<std::uint32_t>(kHead), kTail);
        }
        sequence = static_cast<std::uint16_t>(sequence + 2);
        position += static_cast<std::uint32_t>(kHead + kTail);
        if (tick % 100 == 0) {
            sendto(socket, "PING", 4, 0, reinterpret_cast<sockaddr*>(&client), client_len);
        }
        next_tick += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next_tick);
    }

    sendto(socket, "BYE", 3, 0, reinterpret_cast<sockaddr*>(&client), client_len);
    closesocket(socket);
    WSACleanup();
    std::cout << "Diagnostic pair sender sent " << kTicks
              << " ticks (one reorder, one 300 ms stall) and BYE.\n";
    return 0;
}
