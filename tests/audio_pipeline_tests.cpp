#include "wfas/jitter_buffer.hpp"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool value, const std::string& name) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << name << "\n";
    }
}

constexpr std::size_t kFrameBytes = wfas::kStereoFrameBytes;
constexpr std::size_t kPacketFrames = 480; // 10 ms @ 48 kHz
constexpr double kPi = 3.14159265358979323846;

std::vector<std::uint8_t> tone(std::uint32_t start_frame, std::size_t frames) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(start_frame + i) / 48000.0;
        const std::int16_t s =
            static_cast<std::int16_t>(static_cast<int>(12000.0 * std::sin(2.0 * kPi * 440.0 * t)));
        std::memcpy(&out[i * kFrameBytes], &s, 2);
        std::memcpy(&out[i * kFrameBytes + 2], &s, 2);
    }
    return out;
}

struct Feeder {
    wfas::StreamProcessor& processor;
    std::uint16_t seq = 0;
    std::uint32_t pos = 0;
    std::uint32_t cursor = 0;

    void send(std::size_t frames, bool silence = false) {
        auto pcm = tone(cursor, frames);
        wfas::AudioPacketView view{};
        view.sequence = seq;
        view.sample_position = pos;
        view.silence = silence;
        view.pcm = pcm.data();
        view.pcm_bytes = pcm.size();
        processor.accept(view);
        seq = static_cast<std::uint16_t>((seq + 1u) & 0xffffu);
        pos += static_cast<std::uint32_t>(frames);
        cursor += static_cast<std::uint32_t>(frames);
    }

    // Simulates a packet that never arrived: sequence and sample position move
    // forward, so the next real packet exposes a gap.
    void skip(std::size_t frames) {
        seq = static_cast<std::uint16_t>((seq + 1u) & 0xffffu);
        pos += static_cast<std::uint32_t>(frames);
        cursor += static_cast<std::uint32_t>(frames);
    }
};

std::vector<std::uint8_t> read_frames(wfas::JitterBuffer& buffer, std::size_t frames) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    buffer.read(out.data(), out.size());
    return out;
}

std::int16_t sample_at(const std::vector<std::uint8_t>& pcm, std::size_t frame,
                       int channel = 0) {
    std::int16_t value = 0;
    std::memcpy(&value, &pcm[frame * kFrameBytes + static_cast<std::size_t>(channel) * 2], 2);
    return value;
}

void test_plc_crossfade() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    for (int i = 0; i < 10; ++i) feed.send(kPacketFrames);
    feed.skip(kPacketFrames); // lose one 10 ms packet
    for (int i = 0; i < 10; ++i) feed.send(kPacketFrames);

    const auto stats = processor.stats();
    check(stats.lost_packets == 1, "plc: one lost packet detected");
    check(stats.concealment_events == 1, "plc: one concealment event");
    check(stats.plc_frames == kPacketFrames, "plc: concealed exactly one packet");
    check(stats.plc_fallback_frames == 0, "plc: used continuation, not fallback");
    check(stats.crossfade_frames == 96, "plc: 96-frame crossfade into real audio");

    // Packet 10 is the concealed gap.  Read one extra packet so the
    // crossfade at the start of the following real packet is included.
    std::vector<std::uint8_t> out;
    for (int i = 0; i < 12; ++i) {
        auto chunk = read_frames(buffer, kPacketFrames);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }

    bool nonzero_gap = false;
    for (std::size_t f = 10 * kPacketFrames; f < 11 * kPacketFrames; ++f) {
        if (sample_at(out, f) != 0) { nonzero_gap = true; break; }
    }
    check(nonzero_gap, "plc: concealed gap is not digital silence");

    // The crossfade must avoid a large jump at the real-packet boundary.
    const int boundary_delta =
        std::abs(static_cast<int>(sample_at(out, 11 * kPacketFrames)) -
                 static_cast<int>(sample_at(out, 11 * kPacketFrames - 1)));
    check(boundary_delta < 8000, "plc: crossfade keeps boundary continuous");
}

void test_plc_fade_out_to_silence() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    for (int i = 0; i < 10; ++i) feed.send(kPacketFrames);
    feed.send(kPacketFrames, true); // first silence packet: mirrored + faded tail
    feed.send(kPacketFrames, true); // second: play the faded tail, then zeros

    std::vector<std::uint8_t> out;
    for (int i = 0; i < 12; ++i) {
        auto chunk = read_frames(buffer, kPacketFrames);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }

    const std::size_t tail_start = 11 * kPacketFrames;
    const int first = std::abs(static_cast<int>(sample_at(out, tail_start)));
    const int last = std::abs(static_cast<int>(sample_at(out, tail_start + 95)));
    check(first > 0, "plc: fade-out tail starts at audio level");
    check(last <= 1, "plc: fade-out tail decays to silence");
    check(sample_at(out, tail_start + 96) == 0, "plc: sustained silence is digital zero");

    const auto stats = processor.stats();
    check(stats.silence_packets == 2, "plc: silence packets counted");
    check(stats.plc_fallback_frames == 0, "plc: fade-out used the reference");
}

void test_plc_fallback_without_reference() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    feed.send(kPacketFrames, true);
    feed.skip(kPacketFrames);
    feed.send(kPacketFrames, true);

    const auto stats = processor.stats();
    check(stats.plc_frames == 0, "plc fallback: no continuation without reference");
    check(stats.plc_fallback_frames >= kPacketFrames, "plc fallback: zeros counted");
    check(stats.concealment_events >= 2, "plc fallback: events counted");
}

void test_fast_sender_clock() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    for (int i = 0; i < 12; ++i) feed.send(kPacketFrames); // exactly 120 ms
    for (int i = 0; i < 20; ++i) {
        feed.send(504); // sender clock ~5 % fast
        read_frames(buffer, kPacketFrames);
    }

    const auto stats = buffer.stats();
    check(stats.drift_ratio > 1.0, "drift fast: ratio above 1 drains the buffer");
    check(stats.drift_ratio <= 1.005 + 1e-9, "drift fast: ratio clamped at 1.005");
    check(stats.current_frames > 0, "drift fast: buffer not drained");
    check(stats.current_frames <= stats.target_frames + 1000,
          "drift fast: buffer held near target");
}

void test_slow_sender_clock() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    for (int i = 0; i < 12; ++i) feed.send(kPacketFrames);
    for (int i = 0; i < 20; ++i) {
        read_frames(buffer, kPacketFrames); // consume first so the level drops
        feed.send(456);                     // sender clock ~5 % slow
    }

    const auto stats = buffer.stats();
    check(stats.drift_ratio < 1.0, "drift slow: ratio below 1 refills the buffer");
    check(stats.drift_ratio >= 0.995 - 1e-9, "drift slow: ratio clamped at 0.995");
    check(stats.current_frames > 0, "drift slow: buffer not starved");
    check(stats.current_frames + 1000 >= stats.target_frames,
          "drift slow: buffer held near target");
}

void test_underrun_and_padding_debt() {
    wfas::JitterBuffer buffer(20); // target = 960 frames
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    feed.send(kPacketFrames);
    feed.send(kPacketFrames); // exactly 960 frames
    read_frames(buffer, kPacketFrames);
    read_frames(buffer, kPacketFrames); // now empty
    read_frames(buffer, kPacketFrames); // hard underrun

    auto stats = buffer.stats();
    check(stats.underrun_events >= 1, "underrun: event counted");
    check(stats.underrun_frames >= kPacketFrames, "underrun: missing frames counted");
    check(stats.starvation_events == 1, "underrun: empty queue detected");
    check(stats.padding_debt_frames == 0, "underrun: debt forgiven");

    const auto consumed_before = stats.consumed_frames;
    for (int i = 0; i < 10; ++i) feed.send(kPacketFrames);
    read_frames(buffer, kPacketFrames);

    stats = buffer.stats();
    check(stats.padding_skipped_frames == 0,
          "underrun: recovery audio is not skipped to repay debt");
    check(stats.consumed_frames == consumed_before + kPacketFrames,
          "underrun: recovery consumes real audio");
    check(stats.current_frames > 0, "underrun: playback recovered");
    check(stats.starvation_events == 1, "underrun: no repeated starvation");
}

void test_overrun_bounded() {
    wfas::JitterBuffer buffer(20); // target 960, max 14400 frames
    std::vector<std::uint8_t> burst(40000 * kFrameBytes, 0x22);
    buffer.push(burst.data(), burst.size());

    const auto stats = buffer.stats();
    check(stats.overrun_frames > 0, "overrun: excess frames dropped");
    check(stats.overrun_events > 0, "overrun: event counted");
    check(buffer.queued_frames() <= 14400, "overrun: queue stays bounded");
}

// Regression for the Linux->Windows surplus: a sender that delivers ~2.2 %
// more frames than realtime must be absorbed by smooth crossfaded splices,
// not by repeatedly dropping a front chunk (the audible rustling) and not by
// widening the resampler ratio (which would transpose the music).
void test_fast_sender_surplus_is_spliced() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    for (int i = 0; i < 12; ++i) feed.send(kPacketFrames); // 120 ms

    std::int16_t previous[2]{};
    bool have_previous = false;
    int max_delta = 0;
    int peak = 0;
    std::uint32_t fed_frames = 12 * kPacketFrames;

    constexpr int kIterations = 800; // ~8 s
    for (int i = 0; i < kIterations; ++i) {
        const std::size_t frames = (i % 2) ? 491 : 490; // ~+2.2 %
        feed.send(frames);
        fed_frames += static_cast<std::uint32_t>(frames);
        const auto pcm = read_frames(buffer, kPacketFrames);
        for (std::size_t f = 0; f < kPacketFrames; ++f) {
            std::int16_t frame[2];
            std::memcpy(frame, &pcm[f * kFrameBytes], kFrameBytes);
            for (int c = 0; c < 2; ++c) {
                peak = std::max(peak, std::abs(static_cast<int>(frame[c])));
                if (have_previous) {
                    max_delta = std::max(
                        max_delta, std::abs(static_cast<int>(frame[c]) - previous[c]));
                }
                previous[c] = frame[c];
            }
            have_previous = true;
        }
    }

    const auto stats = buffer.stats();
    const std::uint64_t output_frames =
        static_cast<std::uint64_t>(kIterations) * kPacketFrames;
    const std::uint64_t initial_frames = 12ull * kPacketFrames;
    const std::uint64_t surplus =
        fed_frames - initial_frames - output_frames; // extra during the run

    check(surplus > 0, "surplus: fast sender actually produced extra frames");
    check(stats.overrun_events == 0, "surplus: no hard front-chunk drops");
    check(stats.overrun_frames == 0, "surplus: no hard overrun frames");
    check(stats.splice_events > 0, "surplus: smooth splices were used");
    // The ±0.5 % resampler also absorbs part of the surplus (by design);
    // the remainder must be handled by splices, never by hard drops.
    check(stats.splice_frames + 2500 >= surplus,
          "surplus: splices removed the delivered surplus");
    check(stats.splice_frames > surplus / 2,
          "surplus: splices carry most of the correction");
    check(stats.splice_max_frames <= 960, "surplus: each splice stays short");
    check(stats.current_frames <= stats.target_frames + 2000,
          "surplus: buffer stays near target instead of hitting high-water");
    check(stats.current_frames + 1000 >= stats.target_frames,
          "surplus: buffer is not drained by the correction");
    check(peak <= 12001, "surplus: splice never exceeds the input amplitude");
    check(max_delta < 3000, "surplus: splice causes no audible step");
    check(stats.limiter_frames == 0, "surplus: peak guard not needed");
}

void test_jitter_loss_and_diagnostics() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};

    for (int i = 0; i < 80; ++i) {
        if (i % 13 == 0) {
            feed.skip(kPacketFrames); // isolated loss
        } else if (i % 17 == 0) {
            feed.send(kPacketFrames); // burst arrival
            feed.send(kPacketFrames);
        } else {
            feed.send(kPacketFrames);
        }
        read_frames(buffer, kPacketFrames);
    }

    const auto ps = processor.stats();
    const auto bs = buffer.stats();
    check(ps.lost_packets >= 4, "jitter: losses detected");
    check(ps.concealment_events >= 4, "jitter: concealment inserted");
    check(ps.concealed_frames >= ps.plc_frames, "jitter: concealed total covers PLC frames");
    check(bs.min_frames <= bs.max_frames, "diag: min <= max");
    check(bs.avg_frames >= static_cast<double>(bs.min_frames) &&
          bs.avg_frames <= static_cast<double>(bs.max_frames), "diag: avg within min/max");
    check(bs.drift_ratio >= 0.995 - 1e-9 && bs.drift_ratio <= 1.005 + 1e-9,
          "diag: drift ratio bounded");
    check(bs.current_frames == buffer.queued_frames(), "diag: stats snapshot matches queue");
}
} // namespace

int main() {
    test_plc_crossfade();
    test_plc_fade_out_to_silence();
    test_plc_fallback_without_reference();
    test_fast_sender_clock();
    test_slow_sender_clock();
    test_underrun_and_padding_debt();
    test_overrun_bounded();
    test_fast_sender_surplus_is_spliced();
    test_jitter_loss_and_diagnostics();

    if (failures) {
        std::cerr << failures << " audio pipeline test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All audio pipeline tests passed.\n";
    return 0;
}
