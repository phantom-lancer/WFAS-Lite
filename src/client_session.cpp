#include "wfas/client_session.hpp"

#include "wfas/interval_stats.hpp"
#include "wfas/protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace wfas {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~0ull);

std::string socket_error_text(const char* what) {
    std::ostringstream text;
    text << what << " (WSA error " << WSAGetLastError() << ')';
    return text.str();
}

void send_client_bye(UdpClientSocket& socket) {
    // Three datagrams: UDP is lossy and this is the only way to release the
    // server's unicast session when the client disappears on purpose.
    std::string error;
    for (int i = 0; i < 3; ++i) {
        socket.send_text("CLIENT_BYE", error);
        if (i < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct ReceiveDiagnostics {
    IntervalStats intervals;
    ProcessingStats processing;
    double recv_max_ms{};
    double recv_last_ms{};
    std::uint64_t recv_calls{};
    std::uint64_t recv_timeouts{};
    bool have_audio{};
    Clock::time_point last_audio{};
};

void print_diag_line(const ReceiveDiagnostics& d, const StreamStats& s,
                     const JitterBufferStats& b, std::ostream& out) {
    out << "diag: recv_max=" << std::fixed << std::setprecision(1) << d.recv_max_ms
        << "ms iat_last/max=" << d.intervals.last_ms << "/" << d.intervals.max_ms
        << "ms iat>20/50/100=" << d.intervals.over_20_ms << "/"
        << d.intervals.over_50_ms << "/" << d.intervals.over_100_ms
        << " hist[<1,<3,<7,<12,<20,<50,<200,>=200]";
    for (const auto value : d.intervals.histogram) out << ' ' << value;
    out << " proc_max=" << std::setprecision(0) << d.processing.max_us << "us"
        << " recv_calls/timeouts=" << d.recv_calls << "/" << d.recv_timeouts
        << " reorder=" << s.reordered_packets << " stale=" << s.stale_packets
        << " gaps=" << s.gap_events << " gapmax=" << s.gap_max_packets
        << " payload=" << s.packet_frames_min << "-" << s.packet_frames_max << "f"
        << " shortref=" << s.short_reference_packets << "\n"
        << "underrun detail: debt=" << b.padding_debt_frames
        << "f skip_events=" << b.padding_skipped_events
        << " skip_max=" << b.padding_skip_max_frames
        << "f undebt=" << b.underruns_with_debt
        << " max_missing=" << b.underrun_max_missing << "f"
        << " empty=" << b.starvation_events
        << " reprime=" << b.reprime_events
        << " forgiven=" << b.debt_forgiven_frames << "f";
    if (b.recent_underrun_count) {
        const auto& e = b.recent_underruns[b.recent_underrun_count - 1];
        out << " last: req=" << e.requested_frames
            << " avail=" << e.available_frames
            << " missing=" << e.missing_frames
            << " debt=" << e.debt_before << "->" << e.debt_after
            << " consumed=" << e.consumed_frames << "f";
        if (b.recent_underrun_count >= 2) {
            const auto& prev = b.recent_underruns[b.recent_underrun_count - 2];
            out << " gap=" << std::fixed << std::setprecision(1)
                << (e.wall_ms - prev.wall_ms) << "ms";
        }
    }
    out << "\n";
}

} // namespace

const char* session_end_name(SessionEnd end) {
    switch (end) {
        case SessionEnd::server_bye: return "server BYE";
        case SessionEnd::ping_timeout: return "PING timeout";
        case SessionEnd::socket_error: return "socket error";
        case SessionEnd::handshake_timeout: return "handshake timeout";
        case SessionEnd::busy: return "server busy";
        case SessionEnd::incompatible: return "incompatible protocol";
        case SessionEnd::auth_required: return "authentication required";
        case SessionEnd::audio_error: return "local audio error";
        case SessionEnd::stopped: return "stopped";
    }
    return "unknown";
}

bool session_end_is_fatal(SessionEnd end) {
    return end == SessionEnd::incompatible || end == SessionEnd::auth_required ||
           end == SessionEnd::audio_error;
}

UdpClientSocket::UdpClientSocket() = default;

UdpClientSocket::~UdpClientSocket() { close(); }

bool UdpClientSocket::open(int receive_buffer_bytes, std::string& error) {
    error.clear();
    if (is_open()) return true;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        error = socket_error_text("cannot create UDP socket");
        return false;
    }
    // A large receive buffer is what keeps a PulseAudio backlog dump (several
    // seconds of audio arriving at once) from being dropped by the OS.  Lost
    // audio cannot be repaired by the jitter buffer; it becomes PLC and
    // underruns, which are audible.
    const int requested = std::max(65536, receive_buffer_bytes);
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&requested), sizeof(requested));
    handle_ = static_cast<std::uintptr_t>(sock);
    connected_ = false;
    last_error_code_ = 0;
    return true;
}

void UdpClientSocket::close() {
    if (is_open()) {
        closesocket(static_cast<SOCKET>(handle_));
        handle_ = kInvalidSocket;
    }
    connected_ = false;
}

bool UdpClientSocket::connect_to(const std::string& ip, unsigned short port,
                                 std::string& error) {
    error.clear();
    if (!is_open()) {
        error = "UDP socket is not open";
        return false;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (InetPtonA(AF_INET, ip.c_str(), &peer.sin_addr) != 1) {
        error = "invalid IPv4 address: " + ip;
        return false;
    }
    // Reconnecting an existing UDP socket keeps the same local port, so a
    // session the server still believes in can be released with CLIENT_BYE.
    if (connect(static_cast<SOCKET>(handle_),
                reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) ==
        SOCKET_ERROR) {
        last_error_code_ = WSAGetLastError();
        error = socket_error_text("UDP connect failed");
        connected_ = false;
        return false;
    }
    connected_ = true;
    last_error_code_ = 0;
    return true;
}

bool UdpClientSocket::send_text(const std::string& text, std::string& error) {
    error.clear();
    last_error_code_ = 0;
    if (!is_open() || !connected_) {
        error = "UDP socket is not connected";
        return false;
    }
    const int sent = send(static_cast<SOCKET>(handle_), text.data(),
                          static_cast<int>(text.size()), 0);
    if (sent == SOCKET_ERROR) {
        last_error_code_ = WSAGetLastError();
        error = socket_error_text("UDP send failed");
        return false;
    }
    return true;
}

int UdpClientSocket::receive(char* buffer, int capacity, int timeout_ms,
                             std::string& error) {
    error.clear();
    last_error_code_ = 0;
    if (!is_open() || !connected_) {
        error = "UDP socket is not connected";
        return -1;
    }
    const auto sock = static_cast<SOCKET>(handle_);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(sock, &read_set);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int selected = select(0, &read_set, nullptr, nullptr, &tv);
    if (selected == SOCKET_ERROR) {
        error = socket_error_text("UDP select failed");
        return -1;
    }
    if (selected == 0) return 0;
    const int bytes = recv(sock, buffer, capacity, 0);
    if (bytes == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        last_error_code_ = code;
        if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) return 0;
        // WSAECONNRESET is delivered for an ICMP port-unreachable on a
        // connected UDP socket: the peer is gone.
        error = socket_error_text("UDP receive failed");
        return -1;
    }
    return bytes;
}

SessionReport run_client_session(const SessionConfig& config,
                                 UdpClientSocket& socket, JitterBuffer& buffer,
                                 StreamProcessor& processor, AudioSink& sink,
                                 const std::atomic<bool>& stop,
                                 std::ostream& out) {
    SessionReport report;
    report.target_frames = buffer.stats().target_frames;
    const auto session_start = Clock::now();
    const std::uint64_t rendered_at_start = sink.frames_rendered();

    if (!socket.is_open()) {
        std::string error;
        if (!socket.open(config.socket_receive_buffer_bytes, error)) {
            report.end = SessionEnd::socket_error;
            report.error = error;
            return report;
        }
    }
    if (!socket.connect_to(config.ip, config.port, report.error)) {
        report.end = SessionEnd::socket_error;
        return report;
    }

    // Best effort: if a previous process left a session behind on this server
    // under the same local address, this releases it before HELLO.  A server
    // with no session simply ignores the message.
    {
        std::string ignored;
        socket.send_text("CLIENT_BYE", ignored);
    }

    if (config.verbose) {
        out << "Connecting to " << config.ip << ':' << config.port
            << " (WFAS v2)...\n";
    }

    const std::string hello = hello_message();
    const auto handshake_deadline =
        session_start + std::chrono::milliseconds(config.handshake_timeout_ms);
    auto next_hello = Clock::now();
    bool acknowledged = false;
    bool decided = false;
    int handshake_errors = 0;
    char rx[2048]{};

    while (!stop.load(std::memory_order_acquire) && Clock::now() < handshake_deadline) {
        const auto now = Clock::now();
        if (now >= next_hello) {
            std::string error;
            if (!socket.send_text(hello, error)) {
                report.end = SessionEnd::socket_error;
                report.error = error;
                decided = true;
                break;
            }
            ++report.hello_attempts;
            next_hello = now + std::chrono::milliseconds(500);
        }
        std::string error;
        const int bytes = socket.receive(rx, sizeof(rx), 100, error);
        if (bytes < 0) {
            if (socket.last_error_code() == WSAECONNRESET) {
                // An ICMP port-unreachable from a server that just vanished is
                // delivered once on a connected UDP socket.  It says nothing
                // about the server we are now trying to reach (which may have
                // restarted on the same address), so keep going.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (++handshake_errors >= 5) {
                report.end = SessionEnd::socket_error;
                report.error = error;
                decided = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (bytes == 0) continue;
        const std::string control(rx, rx + bytes);
        if (is_hello_ack_v2(control)) {
            acknowledged = true;
            break;
        }
        if (is_incompatible(control)) {
            report.end = SessionEnd::incompatible;
            report.error = control;
            decided = true;
            break;
        }
        if (is_busy(control)) {
            report.end = SessionEnd::busy;
            report.error = "sender is busy with another unicast client";
            decided = true;
            break;
        }
        if (control.rfind("WFAS_AUTH_REQUIRED", 0) == 0 ||
            control.rfind("WFAS_UNAUTHORIZED", 0) == 0) {
            report.end = SessionEnd::auth_required;
            report.error = control;
            decided = true;
            break;
        }
    }

    if (stop.load(std::memory_order_acquire)) {
        report.end = SessionEnd::stopped;
    } else if (!acknowledged && !decided) {
        report.end = SessionEnd::handshake_timeout;
        report.error = "no HELLO_ACK; check the IP, port and firewall";
    }

    if (!acknowledged) {
        if (report.end == SessionEnd::handshake_timeout) send_client_bye(socket);
    } else {
        report.acknowledged = true;
        // Start every session from a clean playout state: discard any audio
        // left from the previous connection, prebuffer again and fade in.
        processor.reset();
        buffer.reset();
        auto last_ping = Clock::now();
        auto last_liveness = last_ping;
        auto last_report = last_ping;
        ReceiveDiagnostics diag;
        int consecutive_errors = 0;

        while (true) {
            if (stop.load(std::memory_order_acquire)) {
                report.end = SessionEnd::stopped;
                break;
            }
            const auto before_recv = Clock::now();
            std::string error;
            const int bytes = socket.receive(rx, sizeof(rx), 100, error);
            const auto now = Clock::now();
            ++diag.recv_calls;
            diag.recv_last_ms =
                std::chrono::duration<double, std::milli>(now - before_recv).count();
            diag.recv_max_ms = std::max(diag.recv_max_ms, diag.recv_last_ms);

            if (bytes < 0) {
                ++consecutive_errors;
                if (consecutive_errors >= 3) {
                    report.end = SessionEnd::socket_error;
                    report.error = error;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            consecutive_errors = 0;
            if (bytes == 0) ++diag.recv_timeouts;

            if (bytes > 0) {
                AudioPacketView packet{};
                std::string parse_error;
                const auto kind = parse_datagram(
                    reinterpret_cast<const std::uint8_t*>(rx), bytes, packet,
                    parse_error);
                if (kind == ParseResult::audio) {
                    const auto process_start = Clock::now();
                    processor.accept(packet);
                    diag.processing.add_us(
                        std::chrono::duration<double, std::micro>(
                            Clock::now() - process_start)
                            .count());
                    if (diag.have_audio) {
                        const double gap_ms =
                            std::chrono::duration<double, std::milli>(
                                now - diag.last_audio)
                                .count();
                        diag.intervals.add(gap_ms);
                    }
                    diag.last_audio = now;
                    diag.have_audio = true;
                    last_liveness = now;
                } else if (kind == ParseResult::incompatible) {
                    report.end = SessionEnd::incompatible;
                    report.error = parse_error;
                    break;
                } else if (kind == ParseResult::malformed) {
                    // Rate-limit: a broken sender must not flood the console.
                    if (config.verbose && diag.recv_calls % 1000 == 0) {
                        out << "Warning: dropped malformed datagram: "
                            << parse_error << "\n";
                    }
                } else {
                    const std::string control(rx, rx + bytes);
                    if (control == "PING") {
                        last_ping = now;
                        last_liveness = now;
                    } else if (control == "BYE") {
                        if (config.verbose) out << "Sender stopped (BYE).\n";
                        report.end = SessionEnd::server_bye;
                        break;
                    }
                }
            }

            // Liveness: PING is the protocol keep-alive, but audio is equally
            // strong evidence that the sender is alive.  Using only PING made
            // the client disconnect during a burst that overflowed the receive
            // buffer and swallowed a few keep-alives.  A truly dead sender
            // produces neither.
            if (now - last_liveness >
                std::chrono::milliseconds(config.ping_timeout_ms)) {
                report.end = SessionEnd::ping_timeout;
                report.error = "no PING and no audio for " +
                               std::to_string(config.ping_timeout_ms) + " ms";
                break;
            }
            if (!sink.running()) {
                report.end = SessionEnd::audio_error;
                report.error = "WASAPI renderer stopped unexpectedly";
                break;
            }
            if (config.verbose && now - last_report > std::chrono::seconds(5)) {
                const auto s = processor.stats();
                const auto b = buffer.stats();
                out << "stats: audio=" << s.audio_packets
                    << " lost=" << s.lost_packets
                    << " late=" << s.late_packets
                    << " concealed=" << s.concealed_frames << "f"
                    << " plc=" << s.plc_frames << "f"
                    << " plchist=" << s.plc_history_events << "/"
                    << s.plc_history_frames << "f"
                    << " xfade=" << s.crossfade_frames << "f"
                    << " silence=" << s.silence_packets << "p"
                    << " buffer=" << b.current_frames
                    << "f target=" << b.target_frames
                    << "f min=" << b.min_frames
                    << "f avg=" << std::fixed << std::setprecision(1)
                    << b.avg_frames << "f max=" << b.max_frames << "f"
                    << " underrun=" << b.underrun_events << "/"
                    << b.underrun_frames << "f"
                    << " overrun=" << b.overrun_events << "/"
                    << b.overrun_frames << "f"
                    << " splice=" << b.splice_events << "/" << b.splice_frames
                    << "f/max" << b.splice_max_frames << "f"
                    << " skip=" << b.padding_skipped_frames << "f"
                    << " drift=" << std::setprecision(5) << b.drift_ratio
                    << " peak=" << b.peak_sample << "/" << b.raw_peak_sample
                    << " limiter=" << b.limiter_frames << "f"
                    << std::setprecision(6)
                    << " consumed=" << b.consumed_frames
                    << "f rendered=" << (sink.frames_rendered() - rendered_at_start)
                    << "f\n";
                if (b.limiter_frames) {
                    out << "Warning: peak guard engaged (raw peak "
                        << b.raw_peak_sample << ", " << b.limiter_frames
                        << " frames) - please report this.\n";
                }
                print_diag_line(diag, s, b, out);
                last_report = now;
            }
        }

        // Always release the server's unicast session, including on Ctrl+C.
        // A local audio failure is the one case where the socket may already
        // be unusable, but sending is still harmless.
        send_client_bye(socket);
    }

    const auto stats = processor.stats();
    const auto buffered = buffer.stats();
    report.audio_packets = stats.audio_packets;
    report.lost_packets = stats.lost_packets;
    report.late_packets = stats.late_packets;
    report.concealed_frames = stats.concealed_frames;
    report.plc_frames = stats.plc_frames;
    report.crossfade_frames = stats.crossfade_frames;
    report.silence_packets = stats.silence_packets;
    report.underrun_events = buffered.underrun_events;
    report.underrun_frames = buffered.underrun_frames;
    report.overrun_events = buffered.overrun_events;
    report.overrun_frames = buffered.overrun_frames;
    report.splice_events = buffered.splice_events;
    report.splice_frames = buffered.splice_frames;
    report.limiter_frames = buffered.limiter_frames;
    report.peak = buffered.peak_sample;
    report.raw_peak = buffered.raw_peak_sample;
    report.buffer_frames = buffered.current_frames;
    report.drift_ratio = buffered.drift_ratio;
    report.consumed_frames = buffered.consumed_frames;
    report.rendered_frames = sink.frames_rendered() - rendered_at_start;
    report.seconds = std::chrono::duration<double>(Clock::now() - session_start).count();

    if (config.verbose) {
        out << "Session ended (" << session_end_name(report.end) << "): audio="
            << report.audio_packets << ", lost=" << report.lost_packets
            << ", late=" << report.late_packets
            << ", concealed=" << report.concealed_frames << " frames"
            << " (plc=" << report.plc_frames
            << ", crossfade=" << report.crossfade_frames
            << ", silence=" << report.silence_packets << " packets)"
            << ", consumed=" << report.consumed_frames << " frames"
            << ", rendered=" << report.rendered_frames << " frames"
            << ", buffer=" << report.buffer_frames
            << "f target=" << report.target_frames << "f"
            << ", underrun=" << report.underrun_events << " ("
            << report.underrun_frames << "f)"
            << ", overrun=" << report.overrun_events << " ("
            << report.overrun_frames << "f)"
            << ", splice=" << report.splice_events << " ("
            << report.splice_frames << "f)"
            << ", peak=" << report.peak << "/" << report.raw_peak
            << ", limiter=" << report.limiter_frames << "f"
            << ", drift=" << std::setprecision(5) << report.drift_ratio
            << ", duration=" << std::setprecision(1) << report.seconds << "s";
        if (!report.error.empty()) out << ", reason=" << report.error;
        out << ".\n";
    }
    return report;
}

} // namespace wfas
