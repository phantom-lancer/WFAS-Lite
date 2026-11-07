#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wfas {

// Measurement-only helper.  It does not touch the audio path; it only
// classifies already measured intervals / processing times.
struct IntervalStats {
    static constexpr std::size_t kBuckets = 8;

    // Bucket upper bounds in milliseconds:
    // <1, <3, <7, <12, <20, <50, <200, >=200.
    // The same layout is used for the printed histogram.
    static std::size_t bucket_for(double ms) {
        if (ms < 1.0) return 0;
        if (ms < 3.0) return 1;
        if (ms < 7.0) return 2;
        if (ms < 12.0) return 3;
        if (ms < 20.0) return 4;
        if (ms < 50.0) return 5;
        if (ms < 200.0) return 6;
        return 7;
    }

    void add(double ms) {
        ++samples;
        last_ms = ms;
        if (ms > max_ms) max_ms = ms;
        if (ms > 20.0) ++over_20_ms;
        if (ms > 50.0) ++over_50_ms;
        if (ms > 100.0) ++over_100_ms;
        ++histogram[bucket_for(ms)];
    }

    std::array<std::uint64_t, kBuckets> histogram{};
    std::uint64_t samples{};
    std::uint64_t over_20_ms{};
    std::uint64_t over_50_ms{};
    std::uint64_t over_100_ms{};
    double last_ms{};
    double max_ms{};
};

struct ProcessingStats {
    void add_us(double value) {
        ++samples;
        last_us = value;
        if (value > max_us) {
            max_us = value;
            max_us_packet = samples;
        }
    }

    std::uint64_t samples{};
    std::uint64_t max_us_packet{};
    double last_us{};
    double max_us{};
};

} // namespace wfas
