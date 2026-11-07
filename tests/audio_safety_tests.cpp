// Stage A.1 safety tests: output amplitude, clipping/overflow, PLC/crossfade
// transitions, underrun recovery and long-running drift correction.
#include "wfas/jitter_buffer.hpp"
#include <algorithm>
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

std::vector<std::uint8_t> tone(std::uint32_t start_frame, std::size_t frames, double amplitude,
                               double frequency = 440.0) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(start_frame + i) / 48000.0;
        const std::int16_t s =
            static_cast<std::int16_t>(amplitude * std::sin(2.0 * kPi * frequency * t));
        std::memcpy(&out[i * kFrameBytes], &s, 2);
        std::memcpy(&out[i * kFrameBytes + 2], &s, 2);
    }
    return out;
}

struct OutputMetrics {
    int peak = 0;
    int min_sample = 0;
    int max_delta = 0;
    std::size_t clipped = 0;
    std::size_t samples = 0;
    long double sum_sq = 0.0;

    void add(const std::vector<std::uint8_t>& pcm, std::size_t frames) {
        std::int16_t previous[2]{};
        bool have_previous = false;
        for (std::size_t f = 0; f < frames; ++f) {
            std::int16_t frame[2];
            std::memcpy(frame, pcm.data() + f * kFrameBytes, kFrameBytes);
            for (int c = 0; c < 2; ++c) {
                const int value = frame[c];
                peak = std::max(peak, std::abs(value));
                min_sample = std::min(min_sample, value);
                if (value == 32767 || value == -32768) ++clipped;
                if (have_previous) {
                    max_delta = std::max(max_delta, std::abs(value - previous[c]));
                }
                sum_sq += static_cast<long double>(value) * value;
                ++samples;
            }
            previous[0] = frame[0];
            previous[1] = frame[1];
            have_previous = true;
        }
    }

    double rms() const {
        return samples ? static_cast<double>(std::sqrt(sum_sq / samples)) : 0.0;
    }
};

struct Feeder {
    wfas::StreamProcessor& processor;
    std::uint16_t sequence = 0;
    std::uint32_t position = 0;
    std::uint32_t cursor = 0;
    double amplitude = 12000.0;

    void send(std::size_t frames) {
        auto pcm = tone(cursor, frames, amplitude);
        wfas::AudioPacketView view{};
        view.sequence = sequence;
        view.sample_position = position;
        view.pcm = pcm.data();
        view.pcm_bytes = pcm.size();
        processor.accept(view);
        sequence = static_cast<std::uint16_t>((sequence + 1u) & 0xffffu);
        position += static_cast<std::uint32_t>(frames);
        cursor += static_cast<std::uint32_t>(frames);
    }

    void skip(std::size_t frames) {
        sequence = static_cast<std::uint16_t>((sequence + 1u) & 0xffffu);
        position += static_cast<std::uint32_t>(frames);
        cursor += static_cast<std::uint32_t>(frames);
    }
};

std::vector<std::uint8_t> read_frames(wfas::JitterBuffer& buffer, std::size_t frames) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    buffer.read(out.data(), out.size());
    return out;
}

// Feeds 480-frame packets, reads 480-frame blocks.  Returns aggregate output
// metrics for the whole run.
OutputMetrics run(Feeder& feed, wfas::JitterBuffer& buffer, int iterations,
                  int loss_every = 0, int extra_frames_every = 0) {
    OutputMetrics metrics;
    for (int i = 0; i < iterations; ++i) {
        if (loss_every && i % loss_every == loss_every - 1) {
            feed.skip(kPacketFrames);
        } else {
            feed.send(kPacketFrames);
        }
        if (extra_frames_every && i % extra_frames_every == 0) {
            feed.send(1); // sender clock slightly fast
        }
        metrics.add(read_frames(buffer, kPacketFrames), kPacketFrames);
    }
    return metrics;
}

// The phase bug: every positive drift ratio left the resampler position
// outside [0, 1), where it grew by (ratio - 1) per output frame and made the
// interpolator extrapolate thousands of frames ahead (full-scale blast).
void test_positive_drift_does_not_extrapolate() {
    wfas::JitterBuffer buffer(120); // target 5760 frames
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};
    feed.amplitude = 30000.0;

    // Prime well above target so the controller keeps ratio > 1.0 for most of
    // the run; the buffer drains by 0.5 % while the ratio is at its clamp.
    for (int i = 0; i < 17; ++i) feed.send(kPacketFrames); // 8160 frames
    read_frames(buffer, kPacketFrames);

    OutputMetrics metrics;
    double max_drift = 1.0;
    for (int i = 0; i < 800; ++i) { // ~8 s
        feed.send(kPacketFrames);
        metrics.add(read_frames(buffer, kPacketFrames), kPacketFrames);
        max_drift = std::max(max_drift, buffer.stats().drift_ratio);
    }
    const auto stats = buffer.stats();
    check(max_drift > 1.0, "drift: ratio goes above 1.0 while buffer is above target");
    check(max_drift <= 1.005 + 1e-9, "drift: ratio clamped at 1.005");
    check(metrics.peak <= 30001, "drift: output never exceeds the input amplitude");
    check(metrics.clipped == 0, "drift: no full-scale clipping");
    check(metrics.max_delta <= 2500, "drift: no sample-to-sample discontinuity");
    check(stats.limiter_frames == 0, "drift: peak guard must not be needed");
    check(stats.underrun_events == 0, "drift: no underrun during the run");
}

// Full-scale-ish input with regular packet loss: PLC, crossfade and drift must
// stay convex, i.e. never produce a sample outside the input range.
void test_plc_stays_within_input_range() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};
    feed.amplitude = 31000.0;

    for (int i = 0; i < 12; ++i) feed.send(kPacketFrames);
    read_frames(buffer, kPacketFrames);

    const auto metrics = run(feed, buffer, 2000, /*loss_every=*/10, /*extra_frames_every=*/50);
    const auto stats = buffer.stats();
    check(metrics.peak <= 31001, "plc: peak stays within the input amplitude");
    check(metrics.min_sample > -32768, "plc: no -32768 overflow sample");
    check(metrics.clipped == 0, "plc: no full-scale clipping");
    check(stats.limiter_frames == 0, "plc: peak guard must not be needed");
    check(stats.raw_peak_sample <= 31001, "plc: raw (pre-limiter) peak bounded");
    check(processor.stats().plc_frames > 0, "plc: continuation actually used");
}

// A single isolated loss: the PLC block and the crossfade back into real audio
// must not introduce a large step.
void test_plc_transition_continuity() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};
    feed.amplitude = 16000.0;

    for (int i = 0; i < 12; ++i) feed.send(kPacketFrames);
    read_frames(buffer, kPacketFrames);

    OutputMetrics metrics;
    for (int i = 0; i < 5; ++i) {
        feed.send(kPacketFrames);
        metrics.add(read_frames(buffer, kPacketFrames), kPacketFrames);
    }
    feed.skip(kPacketFrames); // isolated loss
    for (int i = 0; i < 16; ++i) {
        feed.send(kPacketFrames);
        metrics.add(read_frames(buffer, kPacketFrames), kPacketFrames);
    }
    // 440 Hz at 16 000 has a maximum per-sample slope of ~920; allow PLC and
    // crossfade to add some headroom, but reject clicks approaching full scale.
    check(metrics.max_delta < 6000, "plc: transitions stay continuous");
    check(metrics.peak <= 16001, "plc: transition peak bounded");
    check(buffer.stats().limiter_frames == 0, "plc: peak guard must not be needed");
}

void test_underrun_recovery() {
    wfas::JitterBuffer buffer(20); // target 960 frames
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};
    feed.amplitude = 20000.0;

    feed.send(kPacketFrames);
    feed.send(kPacketFrames); // exactly the target
    read_frames(buffer, kPacketFrames);
    read_frames(buffer, kPacketFrames); // empties the queue
    const auto starvation = read_frames(buffer, kPacketFrames); // underrun

    auto stats = buffer.stats();
    check(stats.underrun_events == 1, "underrun: exactly one event");
    check(stats.underrun_frames == kPacketFrames, "underrun: missing frames counted");
    check(stats.starvation_events == 1, "underrun: empty queue detected");
    check(stats.padding_debt_frames == 0, "underrun: debt forgiven");
    check(stats.padding_skipped_frames == 0, "underrun: nothing skipped");

    // The underrun output must be silence, never a stale/amplified sample.
    const OutputMetrics silence = [&] {
        OutputMetrics m;
        m.add(starvation, kPacketFrames);
        return m;
    }();
    check(silence.peak == 0, "underrun: output is digital silence");

    for (int i = 0; i < 6; ++i) feed.send(kPacketFrames);
    OutputMetrics recovery;
    for (int i = 0; i < 4; ++i) {
        recovery.add(read_frames(buffer, kPacketFrames), kPacketFrames);
    }

    stats = buffer.stats();
    check(stats.padding_skipped_frames == 0, "underrun: recovery audio is not skipped");
    check(stats.padding_debt_frames == 0, "underrun: debt stays clear");
    check(stats.starvation_events == 1, "underrun: no repeated starvation");
    check(stats.underrun_events == 1, "underrun: recovery does not oscillate");
    check(stats.overrun_events == 0, "underrun: recovery stays below the high-water mark");
    check(recovery.peak <= 20001, "underrun: recovery peak bounded");
    check(recovery.rms() > 1000.0, "underrun: playback actually resumed");
    check(stats.current_frames > 0, "underrun: queue refilled");
    check(stats.limiter_frames == 0, "underrun: peak guard must not be needed");
}

// 60 s of head-banging jitter/loss/drift: the RMS per second must stay close
// to the source RMS.  This is the regression test for "volume grows over time".
void test_long_run_amplitude_stability() {
    wfas::JitterBuffer buffer(120);
    wfas::StreamProcessor processor(buffer);
    Feeder feed{processor};
    feed.amplitude = 20000.0;

    for (int i = 0; i < 12; ++i) feed.send(kPacketFrames);
    read_frames(buffer, kPacketFrames);

    constexpr int kIterations = 6000; // 60 s
    double window_sum = 0.0;
    int window_samples = 0;
    double max_window_rms = 0.0;
    double min_window_rms = 1e9;
    OutputMetrics metrics;
    for (int i = 0; i < kIterations; ++i) {
        if (i % 37 == 0) {
            feed.skip(kPacketFrames); // ~2.7 % loss
        } else {
            feed.send(kPacketFrames + (i % 2)); // average 480.5 frames, slightly fast
        }
        auto pcm = read_frames(buffer, kPacketFrames);
        metrics.add(pcm, kPacketFrames);
        for (std::size_t f = 0; f < kPacketFrames; ++f) {
            std::int16_t frame[2];
            std::memcpy(frame, pcm.data() + f * kFrameBytes, kFrameBytes);
            for (int c = 0; c < 2; ++c) {
                window_sum += static_cast<double>(frame[c]) * frame[c];
                ++window_samples;
            }
        }
        if ((i + 1) % 100 == 0) { // 1 s window
            const double rms = std::sqrt(window_sum / window_samples);
            max_window_rms = std::max(max_window_rms, rms);
            min_window_rms = std::min(min_window_rms, rms);
            window_sum = 0.0;
            window_samples = 0;
        }
    }

    const auto stats = buffer.stats();
    const double nominal_rms = 20000.0 / std::sqrt(2.0);
    check(metrics.peak <= 20001, "long run: peak stays within the input amplitude");
    check(metrics.clipped == 0, "long run: no clipping");
    check(max_window_rms <= nominal_rms * 1.05, "long run: RMS never grows");
    check(min_window_rms >= nominal_rms * 0.5, "long run: no silent window");
    check(stats.limiter_frames == 0, "long run: peak guard must not be needed");
    check(stats.drift_ratio >= 0.995 - 1e-9 && stats.drift_ratio <= 1.005 + 1e-9,
          "long run: drift ratio bounded");
}

// The peak guard is a safety net: verify that it actually engages on a signal
// above its threshold and that it never lets full scale through.
void test_peak_guard_engages() {
    wfas::JitterBuffer buffer(20);
    const std::size_t frames = 1920;
    std::vector<std::uint8_t> hot(frames * kFrameBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int16_t s = (i % 2) ? 32767 : -32768;
        std::memcpy(&hot[i * kFrameBytes], &s, 2);
        std::memcpy(&hot[i * kFrameBytes + 2], &s, 2);
    }
    buffer.push(hot.data(), hot.size());
    OutputMetrics metrics;
    metrics.add(read_frames(buffer, frames), frames);
    const auto stats = buffer.stats();
    check(stats.limiter_frames > 0, "guard: engages above the threshold");
    check(stats.raw_peak_sample >= 32767, "guard: raw peak exposes the source level");
    check(metrics.peak <= 32000, "guard: output limited below full scale");
    check(metrics.clipped == 0, "guard: no clipped full-scale sample");

    // Normal programme material must bypass the guard completely.
    wfas::JitterBuffer quiet(20);
    auto pcm = tone(0, 1920, 10000.0);
    quiet.push(pcm.data(), pcm.size());
    read_frames(quiet, kPacketFrames);
    check(quiet.stats().limiter_frames == 0, "guard: transparent for normal levels");
}

void test_partial_frame_read_is_safe() {
    wfas::JitterBuffer buffer(20);
    auto pcm = tone(60, 1000, 8000.0);
    buffer.push(pcm.data(), pcm.size());

    std::uint8_t tiny[3] = {0x7f, 0x7f, 0x7f};
    buffer.read(tiny, sizeof(tiny)); // fewer bytes than one frame: must zero it
    check(tiny[0] == 0 && tiny[1] == 0 && tiny[2] == 0, "read: partial frame is zeroed");

    std::uint8_t one[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    buffer.read(one, sizeof(one)); // exactly one frame
    std::int16_t left = 0;
    std::int16_t right = 1;
    std::memcpy(&left, one, 2);
    std::memcpy(&right, one + 2, 2);
    check(left == right && left != 0 && std::abs(static_cast<int>(left)) <= 8000,
          "read: one-frame read returns a sane sample");
}
} // namespace

int main() {
    test_positive_drift_does_not_extrapolate();
    test_plc_stays_within_input_range();
    test_plc_transition_continuity();
    test_underrun_recovery();
    test_long_run_amplitude_stability();
    test_peak_guard_engages();
    test_partial_frame_read_is_safe();

    if (failures) {
        std::cerr << failures << " audio safety test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All audio safety tests passed.\n";
    return 0;
}
