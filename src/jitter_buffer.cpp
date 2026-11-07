#include "wfas/jitter_buffer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace wfas {
namespace {
constexpr std::uint64_t kRetuneFrames = 9600; // 200 ms @ 48 kHz
constexpr double kDriftAlpha = 0.1;
// Stage A.4 level control.  A surplus is removed with a short, correlation
// aligned crossfade in the queued audio ahead of the playhead.  Removing a
// whole front chunk at the playhead (the old behaviour) produced an audible
// step every time; a 5 ms equal-power splice is inaudible and, unlike widening
// the resampler ratio, does not transpose the music.
constexpr std::uint64_t kSpliceIntervalFrames = 9600; // evaluate every 200 ms
constexpr std::size_t kSpliceMinDropFrames = 240;     // 5 ms: ignore smaller surplus
constexpr std::size_t kMaxSpliceFrames = 960;         // 20 ms per splice
constexpr std::size_t kSpliceFadeFrames = 240;        // 5 ms crossfade
constexpr std::size_t kTrimFloorFrames = 240;         // hold 5 ms above target
constexpr std::size_t kSpliceDepthFrames = 1920;      // splice 40 ms ahead
constexpr std::size_t kSpliceMinDepthFrames = 480;    // never at the playhead
constexpr double kDriftGain = 0.05;
constexpr double kMinDriftRatio = 0.995;
constexpr double kMaxDriftRatio = 1.005;
// Stage A.3.1: debt is repaid only while the buffer is above target and only
// a few milliseconds per read.  A large one-shot skip was audible and could
// empty the queue again.  The resume fade hides the discontinuity after a
// hard underrun without adding a full re-buffer delay.
constexpr std::size_t kMaxDebtRepayFrames = 240; // 5 ms @ 48 kHz
constexpr std::size_t kResumeFadeFrames = 240;   // 5 ms @ 48 kHz
// A momentary touch of zero resumes immediately; an outage longer than this
// re-buffers to the configured latency so playback does not continue on empty.
constexpr std::uint64_t kReprimeAfterEmptyFrames = 4800; // 100 ms @ 48 kHz
constexpr std::size_t kPLCCrossfadeFrames = 96;
// A concealment reference shorter than 5 ms turns the mirrored loop into an
// audible buzz instead of a plausible continuation, so those gaps stay silent.
constexpr std::size_t kMinPLCReferenceFrames = 240;
// Stage A.3.2: when the last packet alone is too short (the Linux sender's
// 133-frame tail), the previous contiguous packet is prepended to the PLC
// reference.  The combined reference is bounded to 20 ms so the mirror does
// not repeat content from too far in the past.
constexpr std::size_t kMaxPLCHistoryFrames = 960; // 20 ms @ 48 kHz
constexpr std::size_t kMaxPLCHistoryBytes =
    kMaxPLCHistoryFrames * kStereoFrameBytes;
// Peak guard.  It only engages above -0.2 dBFS; normal playback never reaches
// it, so the guard cannot mask a DSP mistake silently.
constexpr float kLimiterThreshold = 32000.0f;
constexpr float kLimiterRelease = 0.99979f; // ~100 ms @ 48 kHz

// Bounded float -> int16 conversion.  A plain static_cast is undefined
// behaviour for NaN/out-of-range values and on x86 yields INT_MIN, which then
// clamps to a full-scale -32768 sample: a very loud click.
std::int16_t clamp_sample(float value) {
    if (!std::isfinite(value)) return 0;
    if (value <= -32768.0f) return -32768;
    if (value >= 32767.0f) return 32767;
    return static_cast<std::int16_t>(value);
}

int best_continuation_frame(const std::int16_t* src, int channels, int ref_frames) {
    if (ref_frames < 4) return 0;
    const int last_base = (ref_frames - 1) * channels;
    const int prev_base = (ref_frames - 2) * channels;
    long long best = std::numeric_limits<long long>::max();
    int best_frame = 0;
    for (int f = 1; f < ref_frames - 2; ++f) {
        long long cost = 0;
        for (int c = 0; c < channels; ++c) {
            const long long v = static_cast<long long>(src[f * channels + c]) - src[last_base + c];
            const long long s =
                (static_cast<long long>(src[(f + 1) * channels + c]) - src[f * channels + c]) -
                (static_cast<long long>(src[last_base + c]) - src[prev_base + c]);
            cost += v * v + 4LL * s * s;
        }
        if (cost < best) { best = cost; best_frame = f; }
    }
    return best_frame;
}

// Mirrored continuation of the last good packet with a linear fade-out tail.
// Returns (total_frames + kPLCXfadeFrames) frames.
std::vector<std::uint8_t> build_concealment(const std::uint8_t* ref,
                                            std::size_t ref_usable,
                                            std::size_t total_frames) {
    constexpr int kChannels = 2;
    constexpr std::size_t kXfade = kPLCCrossfadeFrames;
    const int ref_frames = static_cast<int>(ref_usable / kStereoFrameBytes);
    std::vector<std::int16_t> src(ref_usable / sizeof(std::int16_t));
    std::memcpy(src.data(), ref, ref_usable);

    const int offset = best_continuation_frame(src.data(), kChannels, ref_frames);
    const int period = std::max(2 * (ref_frames - 1), 1);
    const std::size_t out_frames = total_frames + kXfade;
    const std::size_t hold = total_frames;
    std::vector<std::uint8_t> out(out_frames * kStereoFrameBytes, 0);
    auto* dst = reinterpret_cast<std::int16_t*>(out.data());

    for (std::size_t i = 0; i < out_frames; ++i) {
        const int p = static_cast<int>((offset + static_cast<long long>(i)) % period);
        const int idx = p < ref_frames ? p : period - p;
        float gain = 1.0f;
        if (i >= hold) {
            gain = 1.0f - static_cast<float>(i - hold + 1) / static_cast<float>(out_frames - hold);
            if (gain < 0.0f) gain = 0.0f;
        }
        for (int c = 0; c < kChannels; ++c) {
            dst[i * kChannels + c] =
                clamp_sample(static_cast<float>(src[idx * kChannels + c]) * gain);
        }
    }
    return out;
}

// Keep only the newest kMaxPLCHistoryFrames of a frame-aligned reference.
void trim_history_front(std::vector<std::uint8_t>& pcm) {
    if (pcm.size() <= kMaxPLCHistoryBytes) return;
    const std::size_t excess = pcm.size() - kMaxPLCHistoryBytes;
    const std::size_t excess_frames = excess / kStereoFrameBytes;
    pcm.erase(pcm.begin(),
              pcm.begin() + static_cast<std::ptrdiff_t>(
                                excess_frames * kStereoFrameBytes));
}
} // namespace

JitterBuffer::JitterBuffer(unsigned latency_ms)
    : latency_ms_(std::clamp(latency_ms, 20u, 2000u)),
      prebuffer_bytes_((48000u * kStereoFrameBytes * latency_ms_) / 1000u),
      high_bytes_(prebuffer_bytes_ + (48000u * kStereoFrameBytes * 60u) / 1000u),
      max_bytes_(prebuffer_bytes_ + (48000u * kStereoFrameBytes * 280u) / 1000u),
      target_frames_(prebuffer_bytes_ / kStereoFrameBytes),
      high_frames_(high_bytes_ / kStereoFrameBytes),
      max_frames_(max_bytes_ / kStereoFrameBytes) {}

void JitterBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    front_offset_ = queued_bytes_ = 0;
    primed_ = false;
    dropped_frames_ = 0;
    consumed_bytes_ = 0;

    drift_ratio_ = 1.0;
    avg_buffered_frames_ = 0.0;
    frames_since_retune_ = 0;
    frames_since_splice_ = 0;
    resampler_active_ = false;
    resample_pos_ = 0.0;
    have_prev_ = have_curr_ = have_last_output_ = false;
    limiter_gain_ = 1.0f;

    min_frames_stat_ = max_frames_stat_ = 0;
    sum_frames_ = 0.0;
    level_samples_ = 0;
    underrun_events_ = underrun_frames_ = 0;
    overrun_events_ = overrun_frames_ = 0;
    splice_events_ = splice_frames_ = 0;
    splice_max_frames_ = 0;
    padding_debt_frames_ = 0;
    padding_skipped_frames_ = 0;
    padding_skipped_events_ = 0;
    padding_skip_max_frames_ = 0;
    underruns_with_debt_ = 0;
    underrun_max_missing_ = 0;
    starvation_events_ = 0;
    debt_forgiven_frames_ = 0;
    reprime_events_ = 0;
    empty_read_frames_ = 0;
    // Fade the first frames in after a reset/reconnect so a new session never
    // starts with a hard step, even if the first packet begins mid-waveform.
    resume_fade_remaining_ = kResumeFadeFrames;
    recent_underruns_.fill(UnderrunEvent{});
    recent_underrun_count_ = 0;
    peak_sample_ = 0;
    raw_peak_sample_ = 0;
    limiter_frames_ = 0;
}

void JitterBuffer::push_locked(const std::uint8_t* pcm, std::size_t bytes) {
    bytes -= bytes % kStereoFrameBytes;
    if (!bytes) return;
    // A burst larger than the hard maximum is a backlog dump.  Prefer a
    // crossfaded splice inside the queue over cutting the chunk that is about
    // to play; the latter is an immediate full-amplitude step.  The front-drop
    // loop below remains only as a last resort if the splice cannot run.
    if (queued_bytes_ + bytes > max_bytes_) {
        const std::size_t level = queued_bytes_ / kStereoFrameBytes;
        if (level > target_frames_) {
            const std::size_t drop = level - target_frames_;
            trim_splice_locked(drop, std::min(drop, kSpliceFadeFrames));
        }
    }
    while (queued_bytes_ + bytes > max_bytes_ && !chunks_.empty()) {
        const auto remaining = chunks_.front().size() - front_offset_;
        queued_bytes_ -= remaining;
        dropped_frames_ += remaining / kStereoFrameBytes;
        overrun_frames_ += remaining / kStereoFrameBytes;
        ++overrun_events_;
        chunks_.pop_front();
        front_offset_ = 0;
    }
    if (bytes > max_bytes_) { // retain newest bounded tail
        const auto skip = bytes - max_bytes_;
        pcm += skip;
        bytes -= skip;
        dropped_frames_ += skip / kStereoFrameBytes;
        overrun_frames_ += skip / kStereoFrameBytes;
        ++overrun_events_;
    }
    chunks_.emplace_back(pcm, pcm + bytes);
    queued_bytes_ += bytes;
}

void JitterBuffer::push(const std::uint8_t* pcm, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    push_locked(pcm, bytes);
}

void JitterBuffer::push_silence(std::size_t frames) {
    std::vector<std::uint8_t> silence(frames * kStereoFrameBytes, 0);
    push(silence.data(), silence.size());
}

bool JitterBuffer::pop_frame_locked(std::uint8_t* frame) {
    while (!chunks_.empty()) {
        auto& front = chunks_.front();
        const auto available = front.size() - front_offset_;
        if (available < kStereoFrameBytes) {
            queued_bytes_ -= available;
            chunks_.pop_front();
            front_offset_ = 0;
            continue;
        }
        std::memcpy(frame, front.data() + front_offset_, kStereoFrameBytes);
        front_offset_ += kStereoFrameBytes;
        queued_bytes_ -= kStereoFrameBytes;
        consumed_bytes_ += kStereoFrameBytes;
        if (front_offset_ == front.size()) {
            chunks_.pop_front();
            front_offset_ = 0;
        }
        return true;
    }
    return false;
}

std::size_t JitterBuffer::pop_frames_locked(std::uint8_t* out, std::size_t frames) {
    std::size_t done = 0;
    while (done < frames && !chunks_.empty()) {
        auto& front = chunks_.front();
        const auto available = front.size() - front_offset_;
        if (available == 0) {
            chunks_.pop_front();
            front_offset_ = 0;
            continue;
        }
        const auto count = std::min(available, (frames - done) * kStereoFrameBytes);
        std::memcpy(out + done * kStereoFrameBytes, front.data() + front_offset_, count);
        front_offset_ += count;
        queued_bytes_ -= count;
        consumed_bytes_ += count;
        done += count / kStereoFrameBytes;
        if (front_offset_ == front.size()) {
            chunks_.pop_front();
            front_offset_ = 0;
        }
    }
    return done;
}

void JitterBuffer::skip_frames_locked(std::size_t frames) {
    std::size_t skipped = 0;
    while (skipped < frames && !chunks_.empty()) {
        auto& front = chunks_.front();
        const auto available = front.size() - front_offset_;
        if (available == 0) {
            chunks_.pop_front();
            front_offset_ = 0;
            continue;
        }
        const auto count = std::min(available, (frames - skipped) * kStereoFrameBytes);
        front_offset_ += count;
        queued_bytes_ -= count;
        skipped += count / kStereoFrameBytes;
        if (front_offset_ == front.size()) {
            chunks_.pop_front();
            front_offset_ = 0;
        }
    }
}

void JitterBuffer::drop_front_chunk_locked() {
    if (chunks_.empty()) return;
    const auto remaining = chunks_.front().size() - front_offset_;
    queued_bytes_ -= remaining;
    dropped_frames_ += remaining / kStereoFrameBytes;
    overrun_frames_ += remaining / kStereoFrameBytes;
    ++overrun_events_;
    chunks_.pop_front();
    front_offset_ = 0;
}

// Remove `drop_frames` from the queued audio at a point deliberately ahead of
// the playback cursor, joining the two sides with an equal-power crossfade.
// The splice depth keeps the operation away from the frame currently being
// interpolated; the candidate position is chosen to minimise the difference
// between the two sides of the join, so periodic programme material joins with
// almost no phase discontinuity.  Pitch is preserved: this is a short skip,
// not a resampling ratio.
void JitterBuffer::trim_splice_locked(std::size_t drop_frames,
                                      std::size_t fade_frames) {
    const std::size_t total = queued_bytes_ / kStereoFrameBytes;
    if (drop_frames == 0 || total < kSpliceMinDepthFrames + 2) return;
    if (drop_frames >= total) drop_frames = total - 1;

    if (fade_frames == 0) fade_frames = 1;
    fade_frames = std::min(fade_frames, drop_frames);

    // Need at least one frame after the removed block for the tail copy.
    if (total < drop_frames + fade_frames + 1) return;
    const std::size_t max_depth = total - drop_frames - fade_frames;
    const std::size_t min_depth = std::min(kSpliceMinDepthFrames, max_depth);
    const std::size_t max_search = std::min(kSpliceDepthFrames, max_depth);
    if (max_search < min_depth) return;

    // Flatten the deque so the splice can cross chunk boundaries safely.
    std::vector<std::uint8_t> pcm(queued_bytes_);
    std::size_t written = 0;
    bool first = true;
    for (const auto& chunk : chunks_) {
        const std::size_t begin = first ? front_offset_ : 0;
        first = false;
        const std::size_t count = chunk.size() - begin;
        std::memcpy(pcm.data() + written, chunk.data() + begin, count);
        written += count;
    }
    if (written != queued_bytes_) return; // defensive: never splice a partial view

    const auto* src = reinterpret_cast<const std::int16_t*>(pcm.data());
    std::size_t pos = min_depth;
    long long best_cost = std::numeric_limits<long long>::max();
    // 4-frame steps keep the search cheap (~0.2 ms) while still finding a
    // low-discontinuity join for music with a period of a few hundred frames.
    constexpr std::size_t kSearchStep = 4;
    for (std::size_t candidate = min_depth; candidate <= max_search;
         candidate += kSearchStep) {
        long long cost = 0;
        for (std::size_t i = 0; i < fade_frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                const long long d =
                    static_cast<long long>(src[(candidate + i) * 2 + c]) -
                    src[(candidate + drop_frames + i) * 2 + c];
                cost += d * d;
            }
        }
        if (cost < best_cost) {
            best_cost = cost;
            pos = candidate;
        }
    }

    const std::size_t out_frames = total - drop_frames;
    std::vector<std::uint8_t> out(out_frames * kStereoFrameBytes);
    auto* dst = reinterpret_cast<std::int16_t*>(out.data());

    std::memcpy(out.data(), pcm.data(), pos * kStereoFrameBytes);
    for (std::size_t i = 0; i < fade_frames; ++i) {
        // Linear (unity-gain) crossfade.  The join is already matched to the
        // most similar position, so a linear blend is inaudible; unlike an
        // equal-power blend it can never boost two correlated samples above
        // the source amplitude (which would trip the peak guard).
        const float gain_b = static_cast<float>(i + 1) /
                             static_cast<float>(fade_frames + 1);
        const float gain_a = 1.0f - gain_b;
        for (int c = 0; c < 2; ++c) {
            const float a = static_cast<float>(src[(pos + i) * 2 + c]);
            const float b =
                static_cast<float>(src[(pos + drop_frames + i) * 2 + c]);
            dst[(pos + i) * 2 + c] = clamp_sample(a * gain_a + b * gain_b);
        }
    }
    const std::size_t tail_src = pos + drop_frames + fade_frames;
    const std::size_t tail_dst = pos + fade_frames;
    const std::size_t tail_frames = total - tail_src;
    if (tail_frames) {
        std::memcpy(out.data() + tail_dst * kStereoFrameBytes,
                    pcm.data() + tail_src * kStereoFrameBytes,
                    tail_frames * kStereoFrameBytes);
    }

    chunks_.clear();
    chunks_.emplace_back(std::move(out));
    front_offset_ = 0;
    queued_bytes_ = out_frames * kStereoFrameBytes;

    ++splice_events_;
    splice_frames_ += drop_frames;
    splice_max_frames_ = std::max<std::uint32_t>(
        splice_max_frames_, static_cast<std::uint32_t>(drop_frames));
}

void JitterBuffer::retune_locked() {
    const double buffered = static_cast<double>(queued_bytes_ / kStereoFrameBytes);
    avg_buffered_frames_ = avg_buffered_frames_ * (1.0 - kDriftAlpha) + buffered * kDriftAlpha;
    const double target = static_cast<double>(target_frames_);
    const double err = avg_buffered_frames_ - target;
    const double err_ratio = target > 0.0 ? err / target : 0.0;
    const double ratio = 1.0 + err_ratio * kDriftGain;
    drift_ratio_ = std::isfinite(ratio)
                       ? std::clamp(ratio, kMinDriftRatio, kMaxDriftRatio)
                       : 1.0;

    if (!resampler_active_ && drift_ratio_ != 1.0) {
        resampler_active_ = true;
        have_curr_ = false;
        resample_pos_ = 0.0;
        have_prev_ = have_last_output_;
        if (have_prev_) prev_frame_ = last_output_frame_;
    }
}

void JitterBuffer::update_level_locked() {
    const auto level = queued_bytes_ / kStereoFrameBytes;
    if (level_samples_ == 0) {
        min_frames_stat_ = max_frames_stat_ = level;
    } else {
        min_frames_stat_ = std::min(min_frames_stat_, level);
        max_frames_stat_ = std::max(max_frames_stat_, level);
    }
    sum_frames_ += static_cast<double>(level);
    ++level_samples_;
}

void JitterBuffer::apply_limiter_locked(std::uint8_t* pcm, std::size_t frames) {
    auto* samples = reinterpret_cast<std::int16_t*>(pcm);
    for (std::size_t i = 0; i < frames; ++i) {
        const float left = static_cast<float>(samples[i * 2]);
        const float right = static_cast<float>(samples[i * 2 + 1]);
        const float peak = std::max(std::fabs(left), std::fabs(right));
        const float target = peak > kLimiterThreshold ? kLimiterThreshold / peak : 1.0f;
        if (target < limiter_gain_) {
            limiter_gain_ = target; // instantaneous attack
        } else {
            limiter_gain_ = limiter_gain_ * kLimiterRelease + (1.0f - kLimiterRelease);
        }
        if (limiter_gain_ < 1.0f) {
            samples[i * 2] = clamp_sample(left * limiter_gain_);
            samples[i * 2 + 1] = clamp_sample(right * limiter_gain_);
            ++limiter_frames_;
        }
    }
}

void JitterBuffer::read(std::uint8_t* output, std::size_t bytes) {
    std::memset(output, 0, bytes);
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t frames = bytes / kStereoFrameBytes;
    if (!frames) return;

    if (!primed_) {
        if (queued_bytes_ < prebuffer_bytes_) return;
        primed_ = true;
        avg_buffered_frames_ = static_cast<double>(queued_bytes_ / kStereoFrameBytes);
        empty_read_frames_ = 0;
    }

    frames_since_retune_ += frames;
    if (frames_since_retune_ >= kRetuneFrames) {
        frames_since_retune_ %= kRetuneFrames;
        retune_locked();
    }
    frames_since_splice_ += frames;

    // Stage A.4: proportional, pitch-preserving level control.  Trim only the
    // surplus above a small deadband, in 5-20 ms pieces, at a point ~40 ms
    // ahead of the playhead.  This replaces the old "drop one whole front
    // chunk every 250 ms", which cut 7 ms of audio at the playhead and was the
    // audible rustling.  Small residual clock drift (< ~0.5 %) is still left to
    // the resampler; larger surplus never becomes a hard cut.
    if (frames_since_splice_ >= kSpliceIntervalFrames) {
        frames_since_splice_ = 0;
        const std::size_t level = queued_bytes_ / kStereoFrameBytes;
        const std::size_t floor = target_frames_ + kTrimFloorFrames;
        if (level > floor + kSpliceMinDropFrames) {
            const std::size_t drop =
                std::min<std::size_t>(level - floor, kMaxSpliceFrames);
            trim_splice_locked(drop, std::min(drop, kSpliceFadeFrames));
        }
    }

    // Defensive only: push_locked already bounds the queue.  Keep the hard
    // maximum as a last resort so a pathological configuration can never grow
    // memory without limit.
    if (queued_bytes_ / kStereoFrameBytes > max_frames_) {
        drop_front_chunk_locked();
    }

    // Give back silence inserted during an underrun instead of permanently
    // shifting playback forward.  Repay only from the surplus above target:
    // skipping audio while the buffer is already low was what turned one gap
    // into permanent starvation.  Cap each repayment so the skip is inaudible.
    const std::uint64_t debt_at_entry = padding_debt_frames_;
    const std::size_t level_at_entry = queued_bytes_ / kStereoFrameBytes;
    if (padding_debt_frames_ > 0 && level_at_entry > target_frames_) {
        const std::size_t excess = level_at_entry - target_frames_;
        const std::size_t max_repay = std::max<std::size_t>(
            1, std::min<std::size_t>(frames / 2, kMaxDebtRepayFrames));
        const std::size_t skip = std::min<std::size_t>(
            static_cast<std::size_t>(padding_debt_frames_),
            std::min(excess, max_repay));
        if (skip) {
            skip_frames_locked(skip);
            padding_debt_frames_ -= skip;
            padding_skipped_frames_ += skip;
            ++padding_skipped_events_;
            padding_skip_max_frames_ =
                std::max<std::uint64_t>(padding_skip_max_frames_, skip);
        }
    }
    // Measurement only: what is actually available for this render request
    // after debt repayment, before popping.
    const std::size_t available_for_read = queued_bytes_ / kStereoFrameBytes;

    std::size_t produced = 0;
    if (!resampler_active_) {
        produced = pop_frames_locked(output, frames);
    } else {
        if (!std::isfinite(resample_pos_)) resample_pos_ = 0.0;
        if (!have_prev_) {
            if (pop_frame_locked(prev_frame_.data())) {
                have_prev_ = true;
                resample_pos_ = 0.0;
            }
        }
        while (produced < frames && have_prev_) {
            // Normalise the fractional position into [0, 1).  Subtracting only
            // one frame here is wrong: with a ratio above 1.0 the position is
            // left >= 1.0 and then grows by (ratio - 1) every output frame,
            // making the interpolator extrapolate ever further ahead of the
            // real audio (the loud-blast bug).  Consume every whole frame.
            bool starved = false;
            while (resample_pos_ >= 1.0) {
                if (!have_curr_ && !pop_frame_locked(curr_frame_.data())) {
                    starved = true;
                    break;
                }
                have_curr_ = true;
                prev_frame_ = curr_frame_;
                have_curr_ = false;
                resample_pos_ -= 1.0;
            }
            if (starved) break;
            if (!have_curr_) {
                if (!pop_frame_locked(curr_frame_.data())) break;
                have_curr_ = true;
            }
            const float t = static_cast<float>(std::clamp(resample_pos_, 0.0, 1.0));
            std::int16_t a[2];
            std::int16_t b[2];
            std::memcpy(a, prev_frame_.data(), sizeof(a));
            std::memcpy(b, curr_frame_.data(), sizeof(b));
            auto* dst = reinterpret_cast<std::int16_t*>(output + produced * kStereoFrameBytes);
            for (int c = 0; c < 2; ++c) {
                dst[c] = clamp_sample(static_cast<float>(a[c]) +
                                      (static_cast<float>(b[c]) - a[c]) * t);
            }
            ++produced;
            resample_pos_ += drift_ratio_;
        }
    }

    if (produced > 0) empty_read_frames_ = 0;

    // Resume after a hard underrun with a short fade-in.  The alternative
    // (waiting for the full prebuffer again) would add the whole latency as
    // silence after every gap; this hides the discontinuity without doing so.
    if (produced > 0 && resume_fade_remaining_ > 0) {
        const std::size_t fade = std::min(produced, resume_fade_remaining_);
        auto* samples = reinterpret_cast<std::int16_t*>(output);
        for (std::size_t i = 0; i < fade; ++i) {
            const std::size_t elapsed = kResumeFadeFrames - resume_fade_remaining_;
            const float gain = std::min(
                1.0f, static_cast<float>(elapsed + i + 1) /
                          static_cast<float>(kResumeFadeFrames));
            samples[i * 2] = clamp_sample(static_cast<float>(samples[i * 2]) * gain);
            samples[i * 2 + 1] =
                clamp_sample(static_cast<float>(samples[i * 2 + 1]) * gain);
        }
        resume_fade_remaining_ -= fade;
    }

    if (produced > 0) {
        // Track the raw peak before the guard so a regression cannot hide
        // behind the limiter.
        auto* samples = reinterpret_cast<const std::int16_t*>(output);
        for (std::size_t i = 0; i < produced * 2; ++i) {
            const int magnitude = std::abs(static_cast<int>(samples[i]));
            raw_peak_sample_ = std::max<std::uint64_t>(raw_peak_sample_, magnitude);
        }
        apply_limiter_locked(output, produced);
        for (std::size_t i = 0; i < produced * 2; ++i) {
            const int magnitude = std::abs(static_cast<int>(samples[i]));
            peak_sample_ = std::max<std::uint64_t>(peak_sample_, magnitude);
        }
        std::memcpy(last_output_frame_.data(),
                    output + (produced - 1) * kStereoFrameBytes,
                    kStereoFrameBytes);
        have_last_output_ = true;
    }

    if (produced < frames) {
        const auto missing = static_cast<std::uint64_t>(frames - produced);
        ++underrun_events_;
        underrun_frames_ += missing;
        const bool hard_starvation = available_for_read == 0;
        if (hard_starvation) {
            // The queue was completely empty.  Chasing the old timeline by
            // skipping every arriving packet is exactly the loop that made
            // playback silent forever.  Forgive the debt and resume with the
            // next real audio instead.
            debt_forgiven_frames_ += padding_debt_frames_;
            padding_debt_frames_ = 0;
            ++starvation_events_;
            empty_read_frames_ += frames;
            if (empty_read_frames_ >= kReprimeAfterEmptyFrames) {
                // The outage is long enough that resuming on an empty queue
                // would only produce more underruns.  Re-buffer to the
                // configured latency instead.
                primed_ = false;
                ++reprime_events_;
                empty_read_frames_ = 0;
            }
            // A frame captured before the gap must not be interpolated into
            // the resumed stream.
            drift_ratio_ = 1.0;
            avg_buffered_frames_ = 0.0;
            frames_since_retune_ = 0;
            frames_since_splice_ = 0;
            resampler_active_ = false;
            resample_pos_ = 0.0;
            have_prev_ = have_curr_ = have_last_output_ = false;
            resume_fade_remaining_ = kResumeFadeFrames;
        } else {
            padding_debt_frames_ = std::min<std::uint64_t>(
                target_frames_, padding_debt_frames_ + missing);
        }
        underrun_max_missing_ = std::max<std::uint32_t>(
            underrun_max_missing_, static_cast<std::uint32_t>(missing));
        if (debt_at_entry > 0) ++underruns_with_debt_;
        // Measurement only: keep the last few events.  Shift is rare and tiny.
        UnderrunEvent event{};
        event.consumed_frames = consumed_bytes_ / kStereoFrameBytes;
        event.wall_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        event.requested_frames = static_cast<std::uint32_t>(frames);
        event.available_frames = static_cast<std::uint32_t>(available_for_read);
        event.produced_frames = static_cast<std::uint32_t>(produced);
        event.missing_frames = static_cast<std::uint32_t>(missing);
        event.debt_before = static_cast<std::uint32_t>(debt_at_entry);
        event.debt_after = static_cast<std::uint32_t>(padding_debt_frames_);
        if (recent_underrun_count_ < recent_underruns_.size()) {
            recent_underruns_[recent_underrun_count_++] = event;
        } else {
            for (std::size_t i = 1; i < recent_underruns_.size(); ++i)
                recent_underruns_[i - 1] = recent_underruns_[i];
            recent_underruns_.back() = event;
        }
    }

    update_level_locked();
}

std::size_t JitterBuffer::queued_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_bytes_ / kStereoFrameBytes;
}

std::uint64_t JitterBuffer::dropped_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_frames_;
}

std::uint64_t JitterBuffer::consumed_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consumed_bytes_ / kStereoFrameBytes;
}

JitterBufferStats JitterBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    JitterBufferStats out;
    out.current_frames = queued_bytes_ / kStereoFrameBytes;
    out.min_frames = min_frames_stat_;
    out.max_frames = max_frames_stat_;
    out.target_frames = target_frames_;
    out.avg_frames = level_samples_
                         ? sum_frames_ / static_cast<double>(level_samples_)
                         : static_cast<double>(out.current_frames);
    out.underrun_events = underrun_events_;
    out.underrun_frames = underrun_frames_;
    out.overrun_events = overrun_events_;
    out.overrun_frames = overrun_frames_;
    out.splice_events = splice_events_;
    out.splice_frames = splice_frames_;
    out.splice_max_frames = splice_max_frames_;
    out.padding_skipped_frames = padding_skipped_frames_;
    out.consumed_frames = consumed_bytes_ / kStereoFrameBytes;
    out.drift_ratio = drift_ratio_;
    out.peak_sample = peak_sample_;
    out.raw_peak_sample = raw_peak_sample_;
    out.limiter_frames = limiter_frames_;
    out.padding_debt_frames = padding_debt_frames_;
    out.padding_skipped_events = padding_skipped_events_;
    out.padding_skip_max_frames = padding_skip_max_frames_;
    out.underruns_with_debt = underruns_with_debt_;
    out.underrun_max_missing = underrun_max_missing_;
    out.starvation_events = starvation_events_;
    out.debt_forgiven_frames = debt_forgiven_frames_;
    out.reprime_events = reprime_events_;
    out.recent_underruns = recent_underruns_;
    out.recent_underrun_count = recent_underrun_count_;
    return out;
}

void StreamProcessor::reset() {
    expected_sequence_ = -1;
    expected_position_ = 0;
    last_payload_bytes_ = 0;
    last_good_pcm_.clear();
    previous_good_pcm_.clear();
    last_real_end_position_ = 0;
    have_last_real_ = false;
    have_previous_real_ = false;
    conceal_tail_.clear();
    in_silence_ = false;
    stats_ = {};
    missing_sequences_.fill(0);
    missing_count_ = 0;
    missing_head_ = 0;
}

void StreamProcessor::clear_history_chain() {
    previous_good_pcm_.clear();
    last_real_end_position_ = 0;
    have_last_real_ = false;
    have_previous_real_ = false;
}

// Stage A.2 measurement-only bookkeeping: remember which sequence numbers
// were counted as missing so a later arrival can be classified as a reorder
// (it fills a real gap) or as a stale/duplicate packet.
void StreamProcessor::note_missing(std::uint16_t first, std::uint16_t count) {
    if (!count) return;
    const std::size_t stored =
        std::min<std::size_t>(count, static_cast<std::size_t>(kMissingHistory));
    const std::uint16_t skip = static_cast<std::uint16_t>(count - stored);
    for (std::size_t i = 0; i < stored; ++i) {
        const std::uint16_t sequence =
            static_cast<std::uint16_t>(first + skip + static_cast<std::uint16_t>(i));
        if (missing_count_ < kMissingHistory) {
            missing_sequences_[(missing_head_ + missing_count_) % kMissingHistory] = sequence;
            ++missing_count_;
        } else {
            missing_sequences_[missing_head_] = sequence;
            missing_head_ = (missing_head_ + 1) % kMissingHistory;
        }
    }
}

bool StreamProcessor::take_missing(std::uint16_t sequence) {
    for (std::size_t i = 0; i < missing_count_; ++i) {
        const std::size_t index = (missing_head_ + i) % kMissingHistory;
        if (missing_sequences_[index] == sequence) {
            const std::size_t last =
                (missing_head_ + missing_count_ - 1) % kMissingHistory;
            missing_sequences_[index] = missing_sequences_[last];
            --missing_count_;
            if (!missing_count_) missing_head_ = 0;
            return true;
        }
    }
    return false;
}

void StreamProcessor::conceal_gap(std::size_t frames, std::uint32_t gap_start) {
    if (!frames) return;
    frames = std::min<std::size_t>(frames, kMaxConcealFrames);
    const std::size_t total_bytes = frames * kStereoFrameBytes;

    ++stats_.concealment_events;
    stats_.concealed_frames += frames;

    const std::size_t last_usable = last_good_pcm_.size() -
                                    (last_good_pcm_.size() % kStereoFrameBytes);
    const std::uint8_t* ref = nullptr;
    std::size_t ref_usable = 0;
    bool used_history = false;
    std::vector<std::uint8_t> history;

    if (last_usable >= kStereoFrameBytes * kMinPLCReferenceFrames) {
        ref = last_good_pcm_.data();
        ref_usable = last_usable;
    } else if (have_last_real_ && have_previous_real_ &&
               last_real_end_position_ == gap_start &&
               !previous_good_pcm_.empty() && last_usable > 0) {
        // The last packet alone is shorter than 5 ms.  Prepend the previous
        // contiguous packet, but only when its end is exactly the start of
        // this gap.  After a real gap, silence, or reset the chain is broken,
        // so this branch cannot bridge across a discontinuity.
        history.reserve(previous_good_pcm_.size() + last_usable);
        history.insert(history.end(), previous_good_pcm_.begin(),
                       previous_good_pcm_.end());
        history.insert(history.end(), last_good_pcm_.begin(),
                       last_good_pcm_.begin() +
                           static_cast<std::ptrdiff_t>(last_usable));
        trim_history_front(history);
        const std::size_t history_usable =
            history.size() - (history.size() % kStereoFrameBytes);
        if (history_usable >= kStereoFrameBytes * kMinPLCReferenceFrames) {
            ref = history.data();
            ref_usable = history_usable;
            used_history = true;
        }
    }

    if (!ref || ref_usable < kStereoFrameBytes * kMinPLCReferenceFrames) {
        buffer_.push_silence(frames);
        conceal_tail_.clear();
        stats_.plc_fallback_frames += frames;
        return;
    }

    const auto block = build_concealment(ref, ref_usable, frames);
    buffer_.push(block.data(), total_bytes);
    conceal_tail_.assign(block.begin() + static_cast<std::ptrdiff_t>(total_bytes), block.end());
    stats_.plc_frames += frames;
    if (used_history) {
        ++stats_.plc_history_events;
        stats_.plc_history_frames += frames;
    }
}

void StreamProcessor::accept(const AudioPacketView& p) {
    ++stats_.audio_packets;
    const auto frames = static_cast<std::uint32_t>(p.pcm_bytes / kStereoFrameBytes);
    if (frames) {
        if (!stats_.packet_frames_max) {
            stats_.packet_frames_min = frames;
            stats_.packet_frames_max = frames;
        } else {
            stats_.packet_frames_min = std::min(stats_.packet_frames_min, frames);
            stats_.packet_frames_max = std::max(stats_.packet_frames_max, frames);
        }
        if (frames < kMinPLCReferenceFrames) ++stats_.short_reference_packets;
    }
    if (expected_sequence_ >= 0) {
        const auto delta = sequence_delta(static_cast<std::uint16_t>(expected_sequence_), p.sequence);
        if (delta >= 0x8000) {
            ++stats_.late_packets;
            if (take_missing(p.sequence)) ++stats_.reordered_packets;
            else ++stats_.stale_packets;
            return;
        }
        if (delta != 0) {
            ++stats_.gap_events;
            stats_.gap_max_packets = std::max(stats_.gap_max_packets, static_cast<std::uint32_t>(delta));
            stats_.lost_packets += delta;
            note_missing(static_cast<std::uint16_t>(p.sequence - delta), delta);
            std::uint64_t missing = 0;
            if (p.sample_position > expected_position_) missing = p.sample_position - expected_position_;
            else if (last_payload_bytes_) missing = std::size_t(std::min<std::uint16_t>(delta, 8)) * last_payload_bytes_ / kStereoFrameBytes;
            missing = std::min<std::uint64_t>(missing, kMaxConcealFrames);
            const auto gap_start = static_cast<std::uint32_t>(expected_position_);
            conceal_gap(static_cast<std::size_t>(missing), gap_start);
            // A sequence gap ends the contiguous chain: the packet that
            // follows must not be chained with audio from before the gap.
            clear_history_chain();
        }
    }
    expected_sequence_ = (p.sequence + 1u) & 0xffffu;
    expected_position_ = std::uint64_t(p.sample_position) + frames;

    if (p.silence || p.pcm_bytes == 0) {
        const std::size_t last_frames = last_payload_bytes_ / kStereoFrameBytes;
        const std::size_t n = frames ? frames : (last_frames ? last_frames : 480);
        ++stats_.silence_packets;
        stats_.silence_frames += n;
        if (in_silence_) {
            // Play the faded continuation tail before true zeros so sustained
            // silence does not start with a click.
            std::size_t remaining = n;
            if (!conceal_tail_.empty()) {
                const std::size_t tail_frames =
                    std::min<std::size_t>(conceal_tail_.size() / kStereoFrameBytes, n);
                buffer_.push(conceal_tail_.data(), tail_frames * kStereoFrameBytes);
                remaining -= tail_frames;
                conceal_tail_.clear();
            }
            if (remaining) buffer_.push_silence(remaining);
        } else {
            in_silence_ = true;
            // Inserted silence must break the PLC chain.  The fade into the
            // silence may still use the last real packet directly, but not a
            // history that bridges across the silence.
            clear_history_chain();
            conceal_gap(n, p.sample_position); // fade the last good packet into silence
        }
    } else {
        in_silence_ = false;
        if (!conceal_tail_.empty()) {
            std::vector<std::uint8_t> merged(p.pcm, p.pcm + p.pcm_bytes);
            const std::size_t n = std::min(conceal_tail_.size() / kStereoFrameBytes,
                                           p.pcm_bytes / kStereoFrameBytes);
            if (n) {
                auto* dst = reinterpret_cast<std::int16_t*>(merged.data());
                const auto* tail = reinterpret_cast<const std::int16_t*>(conceal_tail_.data());
                for (std::size_t i = 0; i < n; ++i) {
                    const float t = static_cast<float>(i + 1) / static_cast<float>(n + 1);
                    for (int c = 0; c < 2; ++c) {
                        const float a = static_cast<float>(tail[i * 2 + c]);
                        const float b = static_cast<float>(dst[i * 2 + c]);
                        dst[i * 2 + c] = clamp_sample(a * (1.0f - t) + b * t);
                    }
                }
                stats_.crossfade_frames += n;
            }
            buffer_.push(merged.data(), merged.size());
            conceal_tail_.clear();
        } else {
            buffer_.push(p.pcm, p.pcm_bytes);
        }
        const std::size_t usable = p.pcm_bytes - (p.pcm_bytes % kStereoFrameBytes);
        // Stage A.3.2: keep one previous real packet, but only while it is
        // contiguous with the current one.  Silence, gaps and resets clear
        // the chain, so a later PLC cannot splice unrelated audio.
        const bool contiguous = have_last_real_ && usable > 0 &&
                                p.sample_position == last_real_end_position_;
        if (contiguous) {
            previous_good_pcm_ = std::move(last_good_pcm_);
            trim_history_front(previous_good_pcm_);
            have_previous_real_ = !previous_good_pcm_.empty();
        } else {
            previous_good_pcm_.clear();
            have_previous_real_ = false;
        }
        last_good_pcm_.assign(p.pcm, p.pcm + usable);
        last_real_end_position_ =
            static_cast<std::uint32_t>(p.sample_position + frames);
        have_last_real_ = usable > 0;
        if (!have_last_real_) {
            previous_good_pcm_.clear();
            have_previous_real_ = false;
        }
        last_payload_bytes_ = p.pcm_bytes;
    }
    stats_.dropped_buffer_frames = buffer_.dropped_frames();
}
} // namespace wfas
