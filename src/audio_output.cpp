#include "wfas/audio_output.hpp"

#include <chrono>
#include <cstddef>

namespace wfas {

bool NullSink::start(std::string& error) {
    error.clear();
    if (running_.load(std::memory_order_acquire)) return true;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void NullSink::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void NullSink::run() {
    constexpr std::size_t kFramesPerBlock = 480; // 10 ms @ 48 kHz
    constexpr auto kBlock = std::chrono::milliseconds(10);
    std::vector<std::uint8_t> block(kFramesPerBlock * kStereoFrameBytes);
    auto next = std::chrono::steady_clock::now();
    while (running_.load(std::memory_order_acquire)) {
        buffer_.read(block.data(), block.size());
        frames_rendered_.fetch_add(kFramesPerBlock, std::memory_order_relaxed);
        next += kBlock;
        const auto now = std::chrono::steady_clock::now();
        // After a long stall do not try to catch up with a burst of reads.
        if (next < now - std::chrono::milliseconds(100)) next = now;
        std::this_thread::sleep_until(next);
    }
}

} // namespace wfas
