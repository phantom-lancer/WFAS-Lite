#include "wfas/jitter_buffer.hpp"
#include "wfas/protocol.hpp"
#include <array>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void check(bool value, const char* name) { if (!value) { ++failures; std::cerr << "FAIL: " << name << "\n"; } }
}
int main() {
    using namespace wfas;
    const std::array<std::uint8_t, 8> pcm{1,0, 2,0, 3,0, 4,0};
    auto raw = make_audio_packet(65535, 0x01020304u, false, pcm.data(), pcm.size());
    AudioPacketView parsed{}; std::string error;
    check(parse_datagram(raw.data(), raw.size(), parsed, error) == ParseResult::audio, "audio parses");
    check(parsed.sequence == 65535 && parsed.sample_position == 0x01020304u, "big endian header");
    check(parsed.pcm_bytes == pcm.size() && !parsed.silence, "payload view");
    raw[2] = 1; check(parse_datagram(raw.data(), raw.size(), parsed, error) == ParseResult::incompatible, "v1 rejected");
    raw.resize(11); raw[2] = 2; check(parse_datagram(raw.data(), raw.size(), parsed, error) == ParseResult::malformed, "unaligned PCM rejected");
    check(sequence_delta(65535, 0) == 1, "sequence wrap");
    check(sequence_delta(2, 1) >= 0x8000, "late packet arithmetic");
    check(hello_message() == "HELLO_FROM_CLIENT;v=2", "hello exact");
    check(is_hello_ack_v2("HELLO_ACK;v=2") && !is_hello_ack_v2("HELLO_ACK;v=1"), "ack version");
    check(is_incompatible("WFAS_INCOMPATIBLE;v=2") && is_busy("WFAS_BUSY"), "control errors");

    JitterBuffer buffer(20); StreamProcessor processor(buffer);
    const std::array<std::uint8_t, 8> first{9,0,9,0,9,0,9,0};
    auto p1raw = make_audio_packet(10, 100, false, first.data(), first.size());
    auto p3raw = make_audio_packet(12, 106, false, first.data(), first.size()); // exact 4-frame gap
    AudioPacketView p1{}, p3{};
    parse_datagram(p1raw.data(), p1raw.size(), p1, error); parse_datagram(p3raw.data(), p3raw.size(), p3, error);
    processor.accept(p1); processor.accept(p3);
    const auto s = processor.stats();
    check(s.lost_packets == 1 && s.concealed_frames == 4, "sample-position loss concealment");
    processor.accept(p1); check(processor.stats().late_packets == 1, "late packet dropped");
    if (failures) return EXIT_FAILURE;
    std::cout << "All protocol tests passed.\n";
}
