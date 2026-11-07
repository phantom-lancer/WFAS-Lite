#pragma once

#include "wfas/protocol.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace wfas {
struct StreamStats {
    std::uint64_t audio_packets{};
    std::uint64_t lost_packets{};
    std::uint64_t late_packets{};
    std::uint64_t concealed_frames{};
    std::uint64_t malformed_packets{};
    std::uint64_t dropped_buffer_frames{};
    // Stage A packet-loss concealment diagnostics.
    std::uint64_t concealment_events{};
    std::uint64_t plc_frames{};
    std::uint64_t plc_fallback_frames{};
    // Stage A.3.2: short gaps whose PLC reference was extended with the
    // previous contiguous packet.  These events are a subset of plc_frames.
    std::uint64_t plc_history_events{};
    std::uint64_t plc_history_frames{};
    std::uint64_t crossfade_frames{};
    std::uint64_t silence_packets{};
    std::uint64_t silence_frames{};

    // Stage A.2 measurement-only diagnostics.  These do not affect audio:
    // they classify what already happened.
    std::uint64_t gap_events{};          // sequence gaps (delta != 0)
    std::uint64_t reordered_packets{};   // late packet that filled a counted gap
    std::uint64_t stale_packets{};       // late packet that filled no counted gap
    std::uint32_t gap_max_packets{};     // largest sequence gap
    std::uint32_t packet_frames_min{};   // smallest audio payload (frames)
    std::uint32_t packet_frames_max{};   // largest audio payload (frames)
    // Packets shorter than the 5 ms PLC reference minimum.  A gap whose
    // reference is that short is concealed with digital silence.
    std::uint64_t short_reference_packets{};
};

// One measured underrun.  Purely diagnostic; used to show what the buffer
// looked like when it could not fill a render request.
struct UnderrunEvent {
    std::uint64_t consumed_frames{};  // consumed_frames at the moment of the event
    double wall_ms{};                 // steady_clock milliseconds at the event
    std::uint32_t requested_frames{};
    std::uint32_t available_frames{};  // queue after debt repayment, before pop
    std::uint32_t produced_frames{};   // frames actually output for this read
    std::uint32_t missing_frames{};
    std::uint32_t debt_before{};
    std::uint32_t debt_after{};
};

// Snapshot of the playout buffer.  All frame counts are 48 kHz stereo frames.
struct JitterBufferStats {
    std::size_t current_frames{};
    std::size_t min_frames{};
    std::size_t max_frames{};
    std::size_t target_frames{};
    double avg_frames{};
    std::uint64_t underrun_events{};
    std::uint64_t underrun_frames{};
    std::uint64_t overrun_events{};
    std::uint64_t overrun_frames{};
    // Stage A.4: smooth, pitch-preserving level control.  Surplus frames are
    // removed with a correlation-aligned crossfade instead of dropping a whole
    // front chunk at the playhead, so no click is produced.  overrun_* above
    // stays reserved for the hard maximum-buffer safety net.
    std::uint64_t splice_events{};
    std::uint64_t splice_frames{};
    std::uint32_t splice_max_frames{};
    std::uint64_t padding_skipped_frames{};
    std::uint64_t consumed_frames{};
    double drift_ratio{1.0};
    // Output safety diagnostics.
    std::uint64_t peak_sample{};      // highest |sample| written (post limiter)
    std::uint64_t raw_peak_sample{};  // highest |sample| before the limiter
    std::uint64_t limiter_frames{};   // frames touched by the peak guard
    // Stage A.2 measurement-only diagnostics.
    std::uint64_t padding_debt_frames{};       // outstanding latency debt
    std::uint64_t padding_skipped_events{};    // reads that repaid debt
    std::uint64_t padding_skip_max_frames{};   // largest single repayment
    std::uint64_t underruns_with_debt{};       // underruns while debt was pending
    std::uint32_t underrun_max_missing{};      // largest single underrun
    // Stage A.3.1 recovery diagnostics: a completely empty queue forgives the
    // debt and resumes with a short fade-in instead of skipping all arriving
    // audio forever.
    std::uint64_t starvation_events{};         // hard underruns on an empty queue
    std::uint64_t debt_forgiven_frames{};      // debt cancelled by those events
    std::uint64_t reprime_events{};            // sustained outages that re-buffered
    static constexpr std::size_t kRecentUnderruns = 8;
    std::array<UnderrunEvent, kRecentUnderruns> recent_underruns{};
    std::size_t recent_underrun_count{};       // number of entries stored
};

class JitterBuffer {
public:
    explicit JitterBuffer(unsigned latency_ms);
    void reset();
    void push(const std::uint8_t* pcm, std::size_t bytes);
    void push_silence(std::size_t frames);
    // Called only by the WASAPI render thread. Always fills requested bytes.
    void read(std::uint8_t* output, std::size_t bytes);
    std::size_t queued_frames() const;
    unsigned latency_ms() const { return latency_ms_; }
    std::uint64_t dropped_frames() const;
    std::uint64_t consumed_frames() const;
    JitterBufferStats stats() const;

private:
    void push_locked(const std::uint8_t* pcm, std::size_t bytes);
    bool pop_frame_locked(std::uint8_t* frame);
    std::size_t pop_frames_locked(std::uint8_t* out, std::size_t frames);
    void skip_frames_locked(std::size_t frames);
    void drop_front_chunk_locked();
    void trim_splice_locked(std::size_t drop_frames, std::size_t fade_frames);
    void retune_locked();
    void update_level_locked();
    void apply_limiter_locked(std::uint8_t* pcm, std::size_t frames);

    const unsigned latency_ms_;
    const std::size_t prebuffer_bytes_;
    const std::size_t high_bytes_;
    const std::size_t max_bytes_;
    const std::size_t target_frames_;
    const std::size_t high_frames_;
    const std::size_t max_frames_;

    mutable std::mutex mutex_;
    std::deque<std::vector<std::uint8_t>> chunks_;
    std::size_t front_offset_{};
    std::size_t queued_bytes_{};
    bool primed_{};
    std::uint64_t dropped_frames_{};
    std::uint64_t consumed_bytes_{};

    // Continuous clock-drift correction (upstream JitterAudioPlayer model).
    double drift_ratio_{1.0};
    double avg_buffered_frames_{};
    std::uint64_t frames_since_retune_{};
    std::uint64_t frames_since_splice_{}; // smooth level-control cadence
    bool resampler_active_{};
    double resample_pos_{};
    std::array<std::uint8_t, kStereoFrameBytes> prev_frame_{};
    std::array<std::uint8_t, kStereoFrameBytes> curr_frame_{};
    std::array<std::uint8_t, kStereoFrameBytes> last_output_frame_{};
    bool have_prev_{};
    bool have_curr_{};
    bool have_last_output_{};
    float limiter_gain_{1.0f};

    // Diagnostics.
    std::size_t min_frames_stat_{};
    std::size_t max_frames_stat_{};
    double sum_frames_{};
    std::uint64_t level_samples_{};
    std::uint64_t underrun_events_{};
    std::uint64_t underrun_frames_{};
    std::uint64_t overrun_events_{};
    std::uint64_t overrun_frames_{};
    std::uint64_t splice_events_{};
    std::uint64_t splice_frames_{};
    std::uint32_t splice_max_frames_{};
    std::uint64_t padding_debt_frames_{};
    std::uint64_t padding_skipped_frames_{};
    std::uint64_t padding_skipped_events_{};
    std::uint64_t padding_skip_max_frames_{};
    std::uint64_t underruns_with_debt_{};
    std::uint32_t underrun_max_missing_{};
    std::uint64_t starvation_events_{};
    std::uint64_t debt_forgiven_frames_{};
    std::uint64_t reprime_events_{};
    std::uint64_t empty_read_frames_{};
    std::size_t resume_fade_remaining_{};
    std::array<UnderrunEvent, JitterBufferStats::kRecentUnderruns> recent_underruns_{};
    std::size_t recent_underrun_count_{};
    std::uint64_t peak_sample_{};
    std::uint64_t raw_peak_sample_{};
    std::uint64_t limiter_frames_{};
};

class StreamProcessor {
public:
    explicit StreamProcessor(JitterBuffer& buffer) : buffer_(buffer) {}
    void reset();
    void accept(const AudioPacketView& packet);
    StreamStats stats() const { return stats_; }

private:
    static constexpr std::uint32_t kMaxConcealFrames = 9600; // 200 ms @ 48 kHz
    static constexpr std::uint32_t kCrossfadeFrames = 96;
    static constexpr std::size_t kMissingHistory = 64;
    void conceal_gap(std::size_t frames, std::uint32_t gap_start);
    void note_missing(std::uint16_t first, std::uint16_t count);
    bool take_missing(std::uint16_t sequence);
    void clear_history_chain();
    JitterBuffer& buffer_;
    int expected_sequence_ = -1;
    std::uint64_t expected_position_{};
    std::size_t last_payload_bytes_{};
    std::vector<std::uint8_t> last_good_pcm_;
    std::vector<std::uint8_t> previous_good_pcm_;
    std::uint32_t last_real_end_position_{};
    bool have_last_real_{};
    bool have_previous_real_{};
    std::vector<std::uint8_t> conceal_tail_;
    bool in_silence_{};
    StreamStats stats_{};
    // Stage A.2 measurement-only: recently counted-as-missing sequence numbers,
    // used to tell a reordered packet from a duplicate/stale one.
    std::array<std::uint16_t, kMissingHistory> missing_sequences_{};
    std::size_t missing_count_{};
    std::size_t missing_head_{};
};
} // namespace wfas
