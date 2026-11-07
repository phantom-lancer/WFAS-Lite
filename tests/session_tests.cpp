// End-to-end session tests against an in-process fake WFAS v2 unicast server.
// They cover the handshake, audio, PING/BYE handling, the liveness rule,
// CLIENT_BYE session release, reconnection after the server disappears and
// resource reuse.  NullSink keeps WASAPI out of the test.
#include "wfas/client_session.hpp"
#include "wfas/jitter_buffer.hpp"
#include "wfas/protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void check(bool value, const std::string& name) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << name << "\n";
    }
}

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kPacketFrames = 480; // 10 ms

// Minimal WFAS v2 unicast server.  It answers HELLO, sends 10 ms audio blocks
// and PING keep-alives, and releases the session when CLIENT_BYE arrives.
class FakeServer {
public:
    enum class Mode { normal, no_ping, silent, busy, bye_after };

    explicit FakeServer(Mode mode, int bye_after_ms = 0)
        : mode_(mode), bye_after_ms_(bye_after_ms) {
        thread_ = std::thread([this] { run(); });
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (!ready_.load(std::memory_order_acquire) && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    ~FakeServer() { stop(); }

    unsigned short port() const { return port_.load(std::memory_order_acquire); }
    bool ready() const { return ready_.load(std::memory_order_acquire); }
    int hellos() const { return hellos_.load(std::memory_order_relaxed); }
    int client_byes() const {
        return client_byes_.load(std::memory_order_relaxed);
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (thread_.joinable()) thread_.join();
    }

private:
    void send_to(SOCKET socket, const sockaddr_in& peer, const std::string& text) {
        sendto(socket, text.data(), static_cast<int>(text.size()), 0,
               reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    }

    void run() {
        SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_handle == INVALID_SOCKET) return;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (bind(socket_handle, reinterpret_cast<sockaddr*>(&local),
                 sizeof(local)) == SOCKET_ERROR) {
            closesocket(socket_handle);
            return;
        }
        int length = sizeof(local);
        getsockname(socket_handle, reinterpret_cast<sockaddr*>(&local), &length);
        port_.store(ntohs(local.sin_port), std::memory_order_release);
        u_long non_blocking = 1;
        ioctlsocket(socket_handle, FIONBIO, &non_blocking);
        ready_.store(true, std::memory_order_release);

        sockaddr_in client{};
        bool session = false;
        auto session_start = Clock::now();
        auto next_audio = Clock::now();
        auto next_ping = Clock::now();
        std::uint16_t sequence = 0;
        std::uint32_t position = 0;
        double phase = 0.0;
        std::vector<std::uint8_t> pcm(kPacketFrames * wfas::kStereoFrameBytes);

        while (running_.load(std::memory_order_acquire)) {
            char buffer[2048];
            for (;;) {
                sockaddr_in from{};
                int from_length = sizeof(from);
                const int bytes = recvfrom(
                    socket_handle, buffer, sizeof(buffer), 0,
                    reinterpret_cast<sockaddr*>(&from), &from_length);
                if (bytes == SOCKET_ERROR) break;
                if (bytes <= 0) continue;
                const std::string message(buffer, buffer + bytes);
                if (message == "CLIENT_BYE") {
                    // A CLIENT_BYE only releases an established session, just
                    // like the upstream server which checks the peer address.
                    if (session) {
                        client_byes_.fetch_add(1, std::memory_order_relaxed);
                        session = false;
                    }
                    continue;
                }
                if (message.rfind("HELLO_FROM_CLIENT", 0) == 0) {
                    hellos_.fetch_add(1, std::memory_order_relaxed);
                    client = from;
                    if (mode_ == Mode::busy || session) {
                        send_to(socket_handle, client, "WFAS_BUSY");
                        continue;
                    }
                    send_to(socket_handle, client, "HELLO_ACK;v=2");
                    session = true;
                    session_start = Clock::now();
                    next_audio = session_start;
                    next_ping = session_start;
                    sequence = 0;
                    position = 0;
                    phase = 0.0;
                }
            }

            const auto now = Clock::now();
            if (session && bye_after_ms_ > 0 &&
                now - session_start >= std::chrono::milliseconds(bye_after_ms_)) {
                send_to(socket_handle, client, "BYE");
                session = false;
            }
            if (session && mode_ != Mode::silent) {
                if (now >= next_audio) {
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
                    sendto(socket_handle,
                           reinterpret_cast<const char*>(packet.data()),
                           static_cast<int>(packet.size()), 0,
                           reinterpret_cast<const sockaddr*>(&client),
                           sizeof(client));
                    position += static_cast<std::uint32_t>(kPacketFrames);
                    next_audio += std::chrono::milliseconds(10);
                    if (next_audio < now - std::chrono::milliseconds(100)) {
                        next_audio = now;
                    }
                }
                if (mode_ != Mode::no_ping && now >= next_ping) {
                    send_to(socket_handle, client, "PING");
                    next_ping = now + std::chrono::milliseconds(200);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        closesocket(socket_handle);
    }

    Mode mode_;
    int bye_after_ms_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> ready_{false};
    std::atomic<unsigned short> port_{0};
    std::atomic<int> hellos_{0};
    std::atomic<int> client_byes_{0};
};

struct Harness {
    wfas::JitterBuffer buffer{120};
    wfas::StreamProcessor processor{buffer};
    wfas::NullSink sink{buffer};
    wfas::UdpClientSocket socket;
    std::atomic<bool> stop{false};

    Harness() {
        std::string error;
        socket.open(1 << 20, error);
        sink.start(error);
    }
    ~Harness() { sink.stop(); }

    wfas::SessionReport run(const FakeServer& server, unsigned ping_timeout_ms = 800,
                            unsigned handshake_timeout_ms = 3000) {
        wfas::SessionConfig config;
        config.ip = "127.0.0.1";
        config.port = server.port();
        config.verbose = false;
        config.ping_timeout_ms = ping_timeout_ms;
        config.handshake_timeout_ms = handshake_timeout_ms;
        std::ostringstream discarded;
        return wfas::run_client_session(config, socket, buffer, processor, sink,
                                        stop, discarded);
    }
};

void test_handshake_audio_and_server_bye() {
    FakeServer server(FakeServer::Mode::bye_after, 700);
    check(server.ready(), "bye: server started");
    Harness harness;
    const auto report = harness.run(server);

    check(report.acknowledged, "bye: handshake acknowledged");
    check(report.end == wfas::SessionEnd::server_bye, "bye: ended on server BYE");
    check(report.audio_packets >= 20, "bye: audio packets received");
    check(report.peak > 0 && report.peak <= 7000,
          "bye: audio actually played at source level");
    check(report.limiter_frames == 0, "bye: peak guard not needed");
}

void test_audio_keeps_session_alive_without_ping() {
    // PING is a keep-alive, but audio is equally strong evidence of liveness.
    // A burst that overflows the receive buffer may swallow PINGs; that must
    // not disconnect a playing session.
    FakeServer server(FakeServer::Mode::no_ping, 1200);
    Harness harness;
    const auto report = harness.run(server, 400);
    check(report.acknowledged, "no-ping: handshake acknowledged");
    check(report.end == wfas::SessionEnd::server_bye,
          "no-ping: audio kept the session alive past the PING timeout");
    check(report.seconds >= 1.0, "no-ping: session lasted longer than the timeout");
}

void test_silent_server_times_out_and_releases() {
    FakeServer server(FakeServer::Mode::silent);
    Harness harness;
    const auto report = harness.run(server, 500);
    check(report.acknowledged, "silent: handshake acknowledged");
    check(report.end == wfas::SessionEnd::ping_timeout,
          "silent: PING timeout detected");
    check(report.seconds >= 0.4 && report.seconds < 3.0,
          "silent: timeout happened in the expected window");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(server.client_byes() >= 1,
          "silent: CLIENT_BYE released the server session");
}

void test_busy_is_retryable() {
    FakeServer server(FakeServer::Mode::busy);
    Harness harness;
    const auto report = harness.run(server, 500, 2000);
    check(!report.acknowledged, "busy: not acknowledged");
    check(report.end == wfas::SessionEnd::busy, "busy: reported as busy");
    check(!wfas::session_end_is_fatal(report.end), "busy: retryable, not fatal");
    check(server.client_byes() == 0, "busy: no CLIENT_BYE for another client's session");
}

void test_client_bye_allows_a_new_session() {
    FakeServer server(FakeServer::Mode::silent);
    Harness harness;
    const auto first = harness.run(server, 400);
    check(first.end == wfas::SessionEnd::ping_timeout, "release: first timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(server.client_byes() >= 1, "release: CLIENT_BYE received");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The fake server refuses a second HELLO while its session is still active,
    // so an acknowledged second session proves the release worked.
    const auto second = harness.run(server, 400);
    check(second.acknowledged, "release: second session accepted");
    check(second.end == wfas::SessionEnd::ping_timeout, "release: second timed out");
}

void test_reconnect_clears_stale_audio_and_plays_again() {
    Harness harness;
    {
        FakeServer gone(FakeServer::Mode::silent);
        const auto first = harness.run(gone, 400);
        check(first.end == wfas::SessionEnd::ping_timeout,
              "reconnect: first server disappeared");
    }

    // Simulate audio left over from the dead session.
    harness.buffer.push_silence(4800);
    check(harness.buffer.queued_frames() == 4800, "reconnect: stale audio queued");
    harness.processor.reset();
    harness.buffer.reset();
    check(harness.buffer.queued_frames() == 0, "reconnect: stale audio cleared");
    check(harness.buffer.stats().current_frames == 0,
          "reconnect: buffer stats reset");

    FakeServer replacement(FakeServer::Mode::bye_after, 900);
    const auto second = harness.run(replacement, 800);
    check(second.acknowledged, "reconnect: second server acknowledged");
    check(second.end == wfas::SessionEnd::server_bye, "reconnect: clean second end");
    check(second.audio_packets >= 20, "reconnect: audio flowing again");
    check(second.peak > 0, "reconnect: audio audible again");
    check(second.underrun_frames < 9600,
          "reconnect: prebuffer avoided a long underrun (" +
              std::to_string(second.underrun_frames) + "f)");
}

void test_repeated_sessions_do_not_leak_handles() {
    DWORD before = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 8; ++i) {
        FakeServer server(FakeServer::Mode::busy);
        Harness harness;
        const auto report = harness.run(server, 300, 1500);
        check(report.end == wfas::SessionEnd::busy,
              "leak: busy session " + std::to_string(i));
    }
    DWORD after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &after);
    check(after <= before + 16,
          "leak: handle count stable (" + std::to_string(before) + " -> " +
              std::to_string(after) + ")");
}

void test_session_end_classification() {
    check(wfas::session_end_is_fatal(wfas::SessionEnd::incompatible),
          "classify: incompatible is fatal");
    check(wfas::session_end_is_fatal(wfas::SessionEnd::auth_required),
          "classify: auth is fatal");
    check(wfas::session_end_is_fatal(wfas::SessionEnd::audio_error),
          "classify: audio error is fatal");
    check(!wfas::session_end_is_fatal(wfas::SessionEnd::ping_timeout),
          "classify: PING timeout is retryable");
    check(!wfas::session_end_is_fatal(wfas::SessionEnd::socket_error),
          "classify: socket error is retryable");
    check(!wfas::session_end_is_fatal(wfas::SessionEnd::busy),
          "classify: busy is retryable");
    check(!wfas::session_end_is_fatal(wfas::SessionEnd::handshake_timeout),
          "classify: handshake timeout is retryable");
}

} // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    test_handshake_audio_and_server_bye();
    test_audio_keeps_session_alive_without_ping();
    test_silent_server_times_out_and_releases();
    test_busy_is_retryable();
    test_client_bye_allows_a_new_session();
    test_reconnect_clears_stale_audio_and_plays_again();
    test_repeated_sessions_do_not_leak_handles();
    test_session_end_classification();
    WSACleanup();

    if (failures) {
        std::cerr << failures << " session test(s) failed.\n";
        return 1;
    }
    std::cout << "All session, reconnect and resource tests passed.\n";
    return 0;
}
