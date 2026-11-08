#pragma once

#include "wfas/audio_output.hpp"
#include "wfas/jitter_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace wfas {

enum class SessionEnd {
    server_bye,       // the sender sent BYE (clean stop)
    ping_timeout,     // no PING and no audio for the configured timeout
    socket_error,     // ICMP unreachable / send or receive failure
    handshake_timeout,
    busy,             // WFAS_BUSY: another client owns the unicast session
    incompatible,
    auth_required,
    audio_error,      // the local playback device stopped
    stopped           // Ctrl+C / --run-seconds
};

const char* session_end_name(SessionEnd end);
bool session_end_is_fatal(SessionEnd end);

struct SessionConfig {
    std::string ip;
    unsigned short port{9090};
    unsigned latency_ms{120};
    unsigned handshake_timeout_ms{15000};
    unsigned ping_timeout_ms{3000};
    int socket_receive_buffer_bytes{1 << 20};
    // Preferred local UDP port.  A stable value lets a restarted client send
    // CLIENT_BYE from the same source address and release its own stale
    // session; 0 means ephemeral.
    unsigned short preferred_local_port{0};
    bool verbose{true};
};

struct SessionReport {
    SessionEnd end{SessionEnd::socket_error};
    bool acknowledged{};
    unsigned hello_attempts{};
    std::string error;
    double seconds{};
    std::uint64_t audio_packets{};
    std::uint64_t lost_packets{};
    std::uint64_t late_packets{};
    std::uint64_t concealed_frames{};
    std::uint64_t plc_frames{};
    std::uint64_t crossfade_frames{};
    std::uint64_t silence_packets{};
    std::uint64_t underrun_events{};
    std::uint64_t underrun_frames{};
    std::uint64_t overrun_events{};
    std::uint64_t overrun_frames{};
    std::uint64_t splice_events{};
    std::uint64_t splice_frames{};
    std::uint64_t limiter_frames{};
    std::uint64_t peak{};
    std::uint64_t raw_peak{};
    std::size_t buffer_frames{};
    std::size_t target_frames{};
    double drift_ratio{1.0};
    std::uint64_t consumed_frames{};
    std::uint64_t rendered_frames{};
};

// One UDP socket for the whole process.  Keeping the same local address means
// a stale session on the server can be released with CLIENT_BYE even after the
// server stopped answering.
class UdpClientSocket {
public:
    UdpClientSocket();
    ~UdpClientSocket();
    UdpClientSocket(const UdpClientSocket&) = delete;
    UdpClientSocket& operator=(const UdpClientSocket&) = delete;

    bool open(int receive_buffer_bytes, unsigned short preferred_local_port,
              std::string& error);
    void close();
    bool is_open() const { return handle_ != kInvalid; }
    // Local UDP port actually bound (39090 normally, an ephemeral port if it
    // was busy).  A stable port lets a restarted client release its own stale
    // server session with CLIENT_BYE.
    unsigned short local_port() const { return local_port_; }
    bool connect_to(const std::string& ip, unsigned short port,
                    std::string& error);
    bool send_text(const std::string& text, std::string& error);

    // > 0 bytes read, 0 on timeout, -1 on error.
    int receive(char* buffer, int capacity, int timeout_ms, std::string& error);
    // Winsock error from the last send/receive, 0 when the last call was clean.
    int last_error_code() const { return last_error_code_; }

private:
    static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~0ull);
    std::uintptr_t handle_{kInvalid};
    bool connected_{};
    unsigned short local_port_{};
    int last_error_code_{};
};

// Runs one HELLO/ACK session.  The caller owns the socket, buffer, processor
// and sink so they can be reused across reconnects.
SessionReport run_client_session(const SessionConfig& config,
                                 UdpClientSocket& socket, JitterBuffer& buffer,
                                 StreamProcessor& processor, AudioSink& sink,
                                 const std::atomic<bool>& stop,
                                 std::ostream& out);

} // namespace wfas
