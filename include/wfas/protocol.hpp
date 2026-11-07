#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wfas {
constexpr std::uint8_t kVersion = 2;
constexpr std::size_t kHeaderBytes = 10;
constexpr std::size_t kStereoFrameBytes = 4;
constexpr std::uint8_t kSilenceFlag = 0x01;

struct AudioPacketView {
    std::uint16_t sequence{};
    std::uint32_t sample_position{};
    bool silence{};
    const std::uint8_t* pcm{};
    std::size_t pcm_bytes{};
};

enum class ParseResult { audio, control, malformed, incompatible };

ParseResult parse_datagram(const std::uint8_t* data, std::size_t bytes,
                           AudioPacketView& packet, std::string& error);
std::vector<std::uint8_t> make_audio_packet(std::uint16_t sequence,
                                            std::uint32_t sample_position,
                                            bool silence,
                                            const std::uint8_t* pcm,
                                            std::size_t pcm_bytes);
std::string hello_message();
bool is_hello_ack_v2(const std::string& message);
bool is_incompatible(const std::string& message);
bool is_busy(const std::string& message);

// Positive values are packets ahead; values >= 0x8000 represent late/old packets.
std::uint16_t sequence_delta(std::uint16_t expected, std::uint16_t received);
} // namespace wfas
