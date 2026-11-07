// Stage A.2/A.3.2 diagnostic tests.  These pin down the exact mechanisms the
// live statistics expose:
//
//   1. a reordered packet is counted as one "lost" + one "late" packet;
//   2. the Linux WFAS native engine sends 347+133 frame pairs every 10 ms;
//      133 frames alone is shorter than the 5 ms PLC reference minimum, so a
//      gap after such a packet used to become digital silence.  Stage A.3.2
//      extends the reference with the previous contiguous packet (<= 20 ms);
//   3. the late packet is discarded, so the real audio is replaced by a PLC
//      continuation, not by the original delayed packet;
//   4. underruns add padding debt; after Stage A.3.1 it is repaid only from
//      surplus above target and forgiven on a hard empty-queue underrun, so
//      one gap cannot turn into permanent silence.
#include "wfas/interval_stats.hpp"
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
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kShortFrames = 133;  // tail packet of the default Linux sender
constexpr std::size_t kLongFrames = 347;   // first packet of the default Linux sender

std::vector<std::uint8_t> tone_at(std::uint32_t start_frame, std::size_t frames,
                                  double amplitude = 12000.0) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(start_frame + i) / 48000.0;
        const auto value = static_cast<std::int16_t>(
            amplitude * std::sin(2.0 * kPi * 440.0 * t));
        std::memcpy(&out[i * kFrameBytes], &value, 2);
        std::memcpy(&out[i * kFrameBytes + 2], &value, 2);
    }
    return out;
}

std::vector<std::uint8_t> constant_at(std::size_t frames, std::int16_t value) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        std::memcpy(&out[i * kFrameBytes], &value, 2);
        std::memcpy(&out[i * kFrameBytes + 2], &value, 2);
    }
    return out;
}

void send_packet(wfas::StreamProcessor& processor, std::uint16_t sequence,
                 std::uint32_t position, std::size_t frames) {
    const auto pcm = tone_at(position, frames);
    wfas::AudioPacketView view{};
    view.sequence = sequence;
    view.sample_position = position;
    view.pcm = pcm.data();
    view.pcm_bytes = pcm.size();
    processor.accept(view);
}

void send_silence(wfas::StreamProcessor& processor, std::uint16_t sequence,
                  std::uint32_t position) {
    wfas::AudioPacketView view{};
    view.sequence = sequence;
    view.sample_position = position;
    view.silence = true;
    view.pcm = nullptr;
    view.pcm_bytes = 0;
    processor.accept(view);
}

std::vector<std::uint8_t> read_frames(wfas::JitterBuffer& buffer, std::size_t frames) {
    std::vector<std::uint8_t> out(frames * kFrameBytes);
    buffer.read(out.data(), out.size());
    return out;
}

std::int16_t sample_at(const std::vector<std::uint8_t>& pcm, std::size_t frame) {
    std::int16_t value = 0;
    std::memcpy(&value, pcm.data() + frame * kFrameBytes, 2);
    return value;
}

// Pair pattern of the default Linux WFAS 1.2.0 native engine: a 480-frame
// 10 ms capture block is split by maxPayloadBytes=1390 into 347 + 133 frames.
// If the head overtakes the tail, the client counts the head as missing and
// rejects it when it finally arrives.  The packet before the gap is the
// 133-frame tail of the previous pair, so Stage A.3.2 prepends the previous
// contiguous 347-frame head and conceals the gap with PLC instead of silence.
void test_reordered_pair_uses_contiguous_history() {
    wfas::JitterBuffer buffer(20); // target 960 frames: the queue below primes
    wfas::StreamProcessor processor(buffer);

    // A full packet establishes a normal reference first.
    send_packet(processor, 0, 0, 480);
    // Pair 1, in order.
    send_packet(processor, 1, 480, kLongFrames);
    send_packet(processor, 2, 480 + kLongFrames, kShortFrames);
    // Pair 2, reordered: the 133-frame tail arrives before the 347-frame head.
    const std::uint32_t pair2 = 480 + kLongFrames + kShortFrames;
    send_packet(processor, 4, pair2 + kLongFrames, kShortFrames);
    send_packet(processor, 3, pair2, kLongFrames); // late, discarded

    const auto s = processor.stats();
    check(s.lost_packets == 1, "history: counted as one lost packet");
    check(s.late_packets == 1, "history: the delayed packet is counted late");
    check(s.reordered_packets == 1, "history: late packet fills the counted gap");
    check(s.stale_packets == 0, "history: not classified as stale/duplicate");
    check(s.gap_events == 1 && s.gap_max_packets == 1, "history: one one-packet gap");
    check(s.concealment_events == 1, "history: exactly one concealment");
    check(s.concealed_frames == kLongFrames, "history: 347 frames concealed");
    check(s.plc_frames == kLongFrames, "history: continuation used");
    check(s.plc_fallback_frames == 0, "history: no digital silence fallback");
    check(s.plc_history_events == 1, "history: contiguous history used");
    check(s.plc_history_frames == kLongFrames, "history: history covered the gap");
    check(s.crossfade_frames == 96, "history: crossfade into the real tail");
    check(s.short_reference_packets == 2, "history: 133-frame packets are still counted");
    check(s.packet_frames_min == kShortFrames && s.packet_frames_max == 480,
          "history: payload size range measured");

    // Balance: 480 + 347 + 133 real frames + 347 PLC + 133 real frames.
    // The 347-frame late packet never enters the queue.
    check(buffer.queued_frames() == 480 + kLongFrames + kShortFrames + kLongFrames + kShortFrames,
          "history: buffer level is not drained");

    // Verify the hole is exactly the 347 frames between 133-frame packets, and
    // that it now contains continuation audio instead of digital zeros.
    const auto out = read_frames(buffer, buffer.queued_frames());
    const std::size_t hole_start = 480 + kLongFrames + kShortFrames;
    bool hole_has_audio = false;
    for (std::size_t f = hole_start; f < hole_start + kLongFrames; ++f) {
        if (sample_at(out, f) != 0) { hole_has_audio = true; break; }
    }
    check(hole_has_audio, "history: concealed region is not digital silence");
    bool tail_has_audio = false;
    for (std::size_t f = hole_start + kLongFrames; f < out.size() / kFrameBytes; ++f) {
        if (sample_at(out, f) != 0) { tail_has_audio = true; break; }
    }
    check(tail_has_audio, "history: playback continues after the hole");
}

// Same reorder, but with a 480-frame (>= 5 ms) reference: PLC produces a
// continuation, not silence.  This isolates the short-reference threshold as
// the reason the live 347/133 stream turns reorders into silence.
void test_reordered_pair_with_long_reference_uses_plc() {
    wfas::JitterBuffer buffer(20);
    wfas::StreamProcessor processor(buffer);

    send_packet(processor, 0, 0, 480);
    // Reorder the very next pair: tail (133) first, head (347) late.
    send_packet(processor, 2, 480 + kLongFrames, kShortFrames);
    send_packet(processor, 1, 480, kLongFrames);

    const auto s = processor.stats();
    check(s.lost_packets == 1 && s.late_packets == 1, "long ref: reorder counted");
    check(s.reordered_packets == 1, "long ref: classified as reorder");
    check(s.plc_frames == kLongFrames, "long ref: continuation used");
    check(s.plc_fallback_frames == 0, "long ref: no silence fallback");
    check(s.crossfade_frames == 96, "long ref: crossfade into real audio");
    check(s.plc_history_events == 0, "long ref: direct PLC does not need history");
}

// A real loss (no late packet at all) after a short packet must also use the
// previous contiguous packet instead of falling back to silence.
void test_true_loss_after_short_packet_uses_history() {
    wfas::JitterBuffer buffer(80);
    wfas::StreamProcessor processor(buffer);

    send_packet(processor, 0, 0, kLongFrames);
    send_packet(processor, 1, kLongFrames, kShortFrames);
    // seq 2 is the next 347-frame head and is genuinely lost; only its tail
    // (seq 3) arrives.
    send_packet(processor, 3, kLongFrames + kShortFrames + kLongFrames,
                kShortFrames);

    const auto s = processor.stats();
    check(s.lost_packets == 1, "loss history: one lost packet");
    check(s.late_packets == 0, "loss history: no late packet");
    check(s.reordered_packets == 0, "loss history: not a reorder");
    check(s.concealed_frames == kLongFrames, "loss history: 347 frames concealed");
    check(s.plc_frames == kLongFrames, "loss history: continuation used");
    check(s.plc_fallback_frames == 0, "loss history: no silence fallback");
    check(s.plc_history_events == 1, "loss history: history path used");
    check(s.plc_history_frames == kLongFrames, "loss history: history covered the gap");
}

// A sequence gap breaks the chain: a later short packet must not reach back
// across the earlier gap, even if the old packets are still in the buffer.
void test_history_resets_after_sequence_gap() {
    wfas::JitterBuffer buffer(80);
    wfas::StreamProcessor processor(buffer);

    send_packet(processor, 0, 0, 480);
    // seq 1 (head) is lost, tail seq 2 arrives.  The 480-frame packet gives a
    // direct PLC reference.
    send_packet(processor, 2, 480 + kLongFrames, kShortFrames);
    // Now seq 3 (the next head) is lost and seq 4 (its tail) arrives.  The
    // last packet is short, but the previous packet is separated by the first
    // gap, so history must not be used.
    send_packet(processor, 4,
                480 + kLongFrames + kShortFrames + kLongFrames, kShortFrames);

    const auto s = processor.stats();
    check(s.lost_packets == 2, "gap reset: two lost packets");
    check(s.late_packets == 0, "gap reset: no late packets");
    check(s.concealed_frames == 2 * kLongFrames, "gap reset: two 347-frame gaps");
    check(s.plc_frames == kLongFrames, "gap reset: first gap used direct PLC");
    check(s.plc_fallback_frames == kLongFrames, "gap reset: second gap stayed silent");
    check(s.plc_history_events == 0, "gap reset: history not used across a gap");
    check(s.plc_history_frames == 0, "gap reset: no history frames");
}

// Inserted silence breaks the chain.  The fade into silence may use the last
// real packet directly, but a later gap must not bridge across the silence.
void test_silence_breaks_history() {
    wfas::JitterBuffer buffer(80);
    wfas::StreamProcessor processor(buffer);

    send_packet(processor, 0, 0, kLongFrames);
    send_packet(processor, 1, kLongFrames, kShortFrames);
    send_silence(processor, 2, kLongFrames + kShortFrames);
    // A real short packet after the silence starts a fresh chain.
    send_packet(processor, 3, kLongFrames + kShortFrames, kShortFrames);
    // The next 347-frame head is lost; its tail arrives.  There must be no
    // history that bridges over the inserted silence.
    send_packet(processor, 5, kLongFrames + 2 * kShortFrames + kLongFrames,
                kShortFrames);

    const auto s = processor.stats();
    check(s.silence_packets == 1, "silence break: silence packet counted");
    check(s.silence_frames == kShortFrames, "silence break: silence length from last packet");
    check(s.concealment_events == 2, "silence break: fade and gap concealed");
    check(s.plc_frames == 0, "silence break: no PLC across the silence");
    check(s.plc_fallback_frames == kShortFrames + kLongFrames,
          "silence break: both events stayed silent");
    check(s.plc_history_events == 0, "silence break: history path never used");
    check(s.plc_history_frames == 0, "silence break: no history frames");
}

// sample_position is uint32 and wraps.  Continuity and the gap start must still
// match across the wrap.
void test_history_continuity_across_sample_position_wrap() {
    wfas::JitterBuffer buffer(80);
    wfas::StreamProcessor processor(buffer);

    const std::uint32_t wrap_start = 0xFFFFFF00u;
    const std::uint32_t after_head =
        static_cast<std::uint32_t>(wrap_start + kLongFrames);
    const std::uint32_t after_tail =
        static_cast<std::uint32_t>(after_head + kShortFrames);
    const std::uint32_t next_tail =
        static_cast<std::uint32_t>(after_tail + kLongFrames);

    send_packet(processor, 0, wrap_start, kLongFrames);
    send_packet(processor, 1, after_head, kShortFrames);
    // Next pair is reordered across the wrap: tail first, head late.
    send_packet(processor, 3, next_tail, kShortFrames);
    send_packet(processor, 2, after_tail, kLongFrames);

    const auto s = processor.stats();
    check(s.late_packets == 1, "wrap: late head counted");
    check(s.reordered_packets == 1, "wrap: reorder counted");
    check(s.concealed_frames == kLongFrames, "wrap: gap measured across the wrap");
    check(s.plc_frames == kLongFrames, "wrap: continuation used");
    check(s.plc_fallback_frames == 0, "wrap: no silence fallback");
    check(s.plc_history_events == 1, "wrap: history used across the wrap");
    check(s.plc_history_frames == kLongFrames, "wrap: history covered the gap");
}

// A packet that never arrives is a real loss: lost > late and no reorder.
void test_true_loss_is_not_a_reorder() {
    wfas::JitterBuffer buffer(80);
    wfas::StreamProcessor processor(buffer);

    send_packet(processor, 0, 0, 480);
    send_packet(processor, 2, 480 + kLongFrames, kShortFrames); // seq 1 missing

    const auto s = processor.stats();
    check(s.lost_packets == 1, "loss: one packet counted missing");
    check(s.late_packets == 0, "loss: no late packet");
    check(s.reordered_packets == 0, "loss: not classified as reorder");
    check(s.stale_packets == 0, "loss: not classified as stale");
    check(s.plc_fallback_frames == 0 && s.plc_frames == kLongFrames,
          "loss: 480-frame reference still allows PLC");
    check(s.plc_history_events == 0, "loss: direct PLC does not need history");
}

// Duplicate / stale packets are late but do not fill a counted gap.
void test_stale_late_packet_is_classified() {
    wfas::JitterBuffer buffer(80);
    wfas::StreamProcessor processor(buffer);

    send_packet(processor, 0, 0, 480);
    send_packet(processor, 1, 480, 480);
    send_packet(processor, 0, 0, 480); // duplicate of an already played packet

    const auto s = processor.stats();
    check(s.late_packets == 1, "stale: late packet counted");
    check(s.reordered_packets == 0, "stale: not a reorder");
    check(s.stale_packets == 1, "stale: classified as duplicate/stale");
    check(s.lost_packets == 0, "stale: no sequence gap");
}

// Stage A.3.1: after the queue is emptied, debt is forgiven instead of being
// repaid by skipping every arriving packet.  Playback resumes with the next
// real audio, so one gap can no longer turn into permanent silence.
void test_hard_starvation_does_not_loop() {
    wfas::JitterBuffer buffer(20); // target 960 frames
    constexpr std::size_t read_frames_count = 480;

    auto pcm = tone_at(0, 960);
    buffer.push(pcm.data(), pcm.size());
    read_frames(buffer, read_frames_count); // prime and play one block
    read_frames(buffer, read_frames_count); // empty the queue

    read_frames(buffer, read_frames_count); // hard underrun
    auto stats = buffer.stats();
    check(stats.underrun_events == 1, "starvation: initial underrun");
    check(stats.starvation_events == 1, "starvation: empty queue detected");
    check(stats.padding_debt_frames == 0, "starvation: debt forgiven");
    check(stats.padding_skipped_frames == 0, "starvation: nothing skipped");
    const auto& event = stats.recent_underruns[0];
    check(event.available_frames == 0, "starvation: the queue was empty");
    check(event.debt_after == 0, "starvation: event records forgiven debt");

    // Feed exactly one render block per read.  The old code skipped it to
    // repay debt and starved forever; now it must be played.
    const auto consumed_before = stats.consumed_frames;
    for (int i = 0; i < 4; ++i) {
        auto block = tone_at(960 + static_cast<std::uint32_t>(i) * 480, 480);
        buffer.push(block.data(), block.size());
        const auto out = read_frames(buffer, read_frames_count);
        check(sample_at(out, 300) != 0, "starvation: arriving audio is played");
    }
    stats = buffer.stats();
    check(stats.underrun_events == 1, "starvation: no further underruns");
    check(stats.padding_skipped_frames == 0, "starvation: no debt repayment loop");
    check(stats.consumed_frames == consumed_before + 4 * read_frames_count,
          "starvation: consumed advances with real audio");
}

// A hard underrun resumes with a short fade-in, not a click and not a full
// re-buffer of silence.
void test_resume_after_starvation_is_faded() {
    wfas::JitterBuffer buffer(20);
    constexpr std::size_t block = 480;

    auto pcm = tone_at(0, 960);
    buffer.push(pcm.data(), pcm.size());
    read_frames(buffer, block);
    read_frames(buffer, block);
    read_frames(buffer, block); // hard underrun, fade armed

    auto resume = constant_at(block, 12000);
    buffer.push(resume.data(), resume.size());
    const auto out = read_frames(buffer, block);

    const auto first = sample_at(out, 0);
    const auto mid = sample_at(out, 239);
    const auto later = sample_at(out, 300);
    check(first < mid, "fade: output ramps up after starvation");
    check(later == 12000, "fade: full level reached after 5 ms");
    check(mid > 11000, "fade: ramp completes within the fade window");
}

// A sustained outage (here 100 ms of empty reads) re-buffers to the configured
// latency instead of resuming on an empty queue and underrunning repeatedly.
void test_sustained_starvation_reprimes() {
    wfas::JitterBuffer buffer(20); // target 960 frames
    constexpr std::size_t block = 480;

    auto pcm = tone_at(0, 960);
    buffer.push(pcm.data(), pcm.size());
    read_frames(buffer, block);
    read_frames(buffer, block); // empty

    for (int i = 0; i < 10; ++i) read_frames(buffer, block); // 100 ms empty
    auto stats = buffer.stats();
    check(stats.starvation_events == 10, "reprime: sustained empty reads counted");
    check(stats.reprime_events == 1, "reprime: outage triggered a re-buffer");
    check(stats.padding_debt_frames == 0, "reprime: debt stays forgiven");
    check(stats.padding_skipped_frames == 0, "reprime: nothing is skipped");

    auto resume = tone_at(2000, 960);
    buffer.push(resume.data(), resume.size());
    const auto out = read_frames(buffer, block);
    stats = buffer.stats();
    check(stats.current_frames == block, "reprime: prebuffer was rebuilt");
    check(stats.underrun_events == 10, "reprime: re-buffer does not add underruns");
    check(sample_at(out, 300) != 0, "reprime: playback resumed after re-buffer");
}

// Debt must not be repaid while the buffer is below target: that would skip
// real audio and make recovery impossible.
void test_debt_not_repaid_below_target() {
    wfas::JitterBuffer buffer(20); // target 960 frames
    constexpr std::size_t block = 480;

    auto pcm = tone_at(0, 960);
    buffer.push(pcm.data(), pcm.size());
    read_frames(buffer, block);
    read_frames(buffer, block); // empty

    auto partial = tone_at(960, 240);
    buffer.push(partial.data(), partial.size());
    read_frames(buffer, block); // soft underrun: 240 produced, debt 240
    auto stats = buffer.stats();
    check(stats.padding_debt_frames == 240, "debt: soft underrun recorded");
    check(stats.padding_skipped_frames == 0, "debt: not repaid on the partial read");

    auto block_pcm = constant_at(block, 12000);
    buffer.push(block_pcm.data(), block_pcm.size());
    const auto out = read_frames(buffer, block);
    stats = buffer.stats();
    check(stats.padding_skipped_frames == 0, "debt: below-target input is not skipped");
    check(stats.padding_debt_frames == 240, "debt: debt is preserved for later");
    check(sample_at(out, 0) == 12000, "debt: the arriving block is played");
}

// Above target the debt is repaid gradually, capped per read, so the skip is
// inaudible and cannot empty the queue again.
void test_debt_repayment_is_gradual() {
    wfas::JitterBuffer buffer(20); // target 960 frames
    constexpr std::size_t block = 480;

    auto pcm = tone_at(0, 960);
    buffer.push(pcm.data(), pcm.size());
    read_frames(buffer, block);
    read_frames(buffer, block); // empty

    // Two partial blocks create 480 frames of debt without hitting zero after
    // the debt was recorded.
    for (int i = 0; i < 2; ++i) {
        auto partial = tone_at(960 + i * 240, 240);
        buffer.push(partial.data(), partial.size());
        read_frames(buffer, block);
    }
    auto stats = buffer.stats();
    check(stats.padding_debt_frames == 480, "gradual: debt accumulated");

    // Push a healthy surplus above target.
    auto surplus = tone_at(2000, 1920);
    buffer.push(surplus.data(), surplus.size());
    read_frames(buffer, block);

    stats = buffer.stats();
    check(stats.padding_skipped_frames == 240,
          "gradual: repayment capped at 5 ms per read");
    check(stats.padding_skip_max_frames == 240, "gradual: max skip recorded");
    check(stats.padding_debt_frames == 240, "gradual: debt repaid in steps");

    read_frames(buffer, block);
    stats = buffer.stats();
    check(stats.padding_skipped_frames == 480, "gradual: second step completes");
    check(stats.padding_debt_frames == 0, "gradual: debt cleared");
}

// One outage drains the queue, then recovery audio arrives.  A hard underrun
// must not start a debt-skip cycle: the new audio is played immediately.
void test_outage_and_recovery_accounting() {
    wfas::JitterBuffer buffer(100); // target 4800 frames
    constexpr std::size_t block = 480;

    auto prepush = tone_at(0, 11 * block);
    buffer.push(prepush.data(), prepush.size());
    read_frames(buffer, block); // prime, 4800 frames left
    for (int i = 0; i < 10; ++i) read_frames(buffer, block); // empty exactly
    check(buffer.stats().underrun_events == 0, "outage: draining is not an underrun");

    read_frames(buffer, block); // the outage becomes a hard underrun
    auto stats = buffer.stats();
    check(stats.underrun_events == 1, "outage: one underrun event");
    check(stats.underrun_frames == block, "outage: one block missing");
    check(stats.starvation_events == 1, "outage: empty queue recorded");
    check(stats.padding_debt_frames == 0, "outage: debt forgiven");
    const auto& event = stats.recent_underruns[0];
    check(event.available_frames == 0, "outage: queue was empty");
    check(event.missing_frames == block, "outage: event missing count");
    check(event.debt_after == 0, "outage: event records forgiven debt");

    auto recovery = tone_at(11 * block, 3 * block);
    buffer.push(recovery.data(), recovery.size());
    const auto out = read_frames(buffer, block);
    stats = buffer.stats();
    check(stats.underrun_events == 1, "outage: recovery does not add underruns");
    check(stats.padding_skipped_frames == 0, "outage: recovery audio is not skipped");
    check(stats.padding_debt_frames == 0, "outage: debt remains clear");
    check(stats.starvation_events == 1, "outage: no repeated starvation");
    check(stats.current_frames == 2 * block, "outage: queue refilled");
    check(sample_at(out, 300) != 0, "outage: playback resumed");
}

void test_interval_histogram() {
    wfas::IntervalStats stats;
    stats.add(0.5);   // <1
    stats.add(2.9);   // <3
    stats.add(5.0);   // <7
    stats.add(10.0);  // <12
    stats.add(15.0);  // <20
    stats.add(30.0);  // <50
    stats.add(100.0); // <200
    stats.add(250.0); // >=200

    check(stats.samples == 8, "interval: sample count");
    check(stats.max_ms == 250.0, "interval: maximum");
    check(stats.over_20_ms == 3, "interval: over 20 ms");
    check(stats.over_50_ms == 2, "interval: over 50 ms");
    check(stats.over_100_ms == 1, "interval: over 100 ms");
    for (std::size_t i = 0; i < wfas::IntervalStats::kBuckets; ++i) {
        check(stats.histogram[i] == 1, "interval: one sample per bucket");
    }
}

} // namespace

int main() {
    test_reordered_pair_uses_contiguous_history();
    test_reordered_pair_with_long_reference_uses_plc();
    test_true_loss_after_short_packet_uses_history();
    test_history_resets_after_sequence_gap();
    test_silence_breaks_history();
    test_history_continuity_across_sample_position_wrap();
    test_true_loss_is_not_a_reorder();
    test_stale_late_packet_is_classified();
    test_hard_starvation_does_not_loop();
    test_resume_after_starvation_is_faded();
    test_sustained_starvation_reprimes();
    test_debt_not_repaid_below_target();
    test_debt_repayment_is_gradual();
    test_outage_and_recovery_accounting();
    test_interval_histogram();

    if (failures) {
        std::cerr << failures << " network diagnostic test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All network diagnostic tests passed.\n";
    return 0;
}
