#pragma once

#include "wfas/jitter_buffer.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace wfas {
class WasapiRenderer {
public:
    explicit WasapiRenderer(JitterBuffer& buffer) : buffer_(buffer) {}
    ~WasapiRenderer();
    WasapiRenderer(const WasapiRenderer&) = delete;
    WasapiRenderer& operator=(const WasapiRenderer&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    std::uint64_t frames_rendered() const { return frames_rendered_.load(std::memory_order_relaxed); }
    // Endpoint buffer size in frames, valid after a successful start().
    unsigned endpoint_frames() const { return endpoint_frames_.load(std::memory_order_relaxed); }

#ifdef WFAS_LITE_TESTING
    // Test-only hook: forces the next start() to fail before touching WASAPI.
    void fail_next_start_for_testing(std::string reason);
#endif

private:
    void run();
    void complete_startup(const std::string& error);

    JitterBuffer& buffer_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<unsigned> endpoint_frames_{0};
    std::mutex lifecycle_mutex_;
    std::mutex state_mutex_;
    std::condition_variable startup_cv_;
    bool startup_complete_{false};
    std::string startup_error_;
    std::thread thread_;
#ifdef WFAS_LITE_TESTING
    std::string forced_startup_failure_;
#endif
};
} // namespace wfas
