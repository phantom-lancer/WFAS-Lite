#include "wfas/protocol.hpp"
#include <algorithm>
#include <cstring>

namespace wfas {
namespace {
std::uint16_t read_u16be(const std::uint8_t* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t read_u32be(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
void write_u16be(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v >> 8); p[1] = std::uint8_t(v); }
void write_u32be(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16);
    p[2] = std::uint8_t(v >> 8); p[3] = std::uint8_t(v);
}
bool starts_with(const std::string& value, const char* prefix) {
    const auto n = std::strlen(prefix);
    return value.size() >= n && value.compare(0, n, prefix) == 0;
}
}

ParseResult parse_datagram(const std::uint8_t* data, std::size_t bytes,
                           AudioPacketView& packet, std::string& error) {
    error.clear();
    if (!data || bytes == 0) { error = "empty UDP datagram"; return ParseResult::malformed; }
    if (bytes < 2 || data[0] != 'W' || data[1] != 'F') return ParseResult::control;
    if (bytes < kHeaderBytes) { error = "WF audio datagram shorter than 10-byte header"; return ParseResult::malformed; }
    if (data[2] != kVersion) { error = "WFAS protocol version is not 2"; return ParseResult::incompatible; }
    const auto payload = bytes - kHeaderBytes;
    if (payload % kStereoFrameBytes != 0) { error = "PCM payload is not aligned to stereo S16LE frames"; return ParseResult::malformed; }
    packet.sequence = read_u16be(data + 4);
    packet.sample_position = read_u32be(data + 6);
    packet.silence = (data[3] & kSilenceFlag) != 0;
    packet.pcm = data + kHeaderBytes;
    packet.pcm_bytes = payload;
    return ParseResult::audio;
}

std::vector<std::uint8_t> make_audio_packet(std::uint16_t sequence, std::uint32_t position,
                                            bool silence, const std::uint8_t* pcm, std::size_t bytes) {
    std::vector<std::uint8_t> out(kHeaderBytes + bytes);
    out[0] = 'W'; out[1] = 'F'; out[2] = kVersion; out[3] = silence ? kSilenceFlag : 0;
    write_u16be(out.data() + 4, sequence); write_u32be(out.data() + 6, position);
    if (bytes && pcm) std::memcpy(out.data() + kHeaderBytes, pcm, bytes);
    return out;
}
std::string hello_message() { return "HELLO_FROM_CLIENT;v=2"; }
bool is_hello_ack_v2(const std::string& message) { return starts_with(message, "HELLO_ACK") && message.find("v=2") != std::string::npos; }
bool is_incompatible(const std::string& message) { return starts_with(message, "WFAS_INCOMPATIBLE"); }
bool is_busy(const std::string& message) { return message == "WFAS_BUSY"; }
std::uint16_t sequence_delta(std::uint16_t expected, std::uint16_t received) { return std::uint16_t(received - expected); }
} // namespace wfas
