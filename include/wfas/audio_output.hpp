#pragma once

#include "wfas/jitter_buffer.hpp"
#include "wfas/wasapi_renderer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wfas {

// The playback device is created once per process and kept alive across
// reconnects.  Recreating a WASAPI stream on every network hiccup caused
// needless device churn and a fresh start-up glitch.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual std::uint64_t frames_rendered() const = 0;
    virtual unsigned endpoint_frames() const = 0;
};

class WasapiSink final : public AudioSink {
public:
    explicit WasapiSink(JitterBuffer& buffer) : renderer_(buffer) {}
    bool start(std::string& error) override { return renderer_.start(error); }
    void stop() override { renderer_.stop(); }
    bool running() const override { return renderer_.running(); }
    std::uint64_t frames_rendered() const override {
        return renderer_.frames_rendered();
    }
    unsigned endpoint_frames() const override {
        return renderer_.endpoint_frames();
    }

private:
    WasapiRenderer renderer_;
};

// Test/headless sink: consumes the jitter buffer at real time without touching
// WASAPI.  Used by the reconnect integration tests (--no-audio).
class NullSink final : public AudioSink {
public:
    explicit NullSink(JitterBuffer& buffer) : buffer_(buffer) {}
    ~NullSink() override { stop(); }
    bool start(std::string& error) override;
    void stop() override;
    bool running() const override { return running_.load(std::memory_order_acquire); }
    std::uint64_t frames_rendered() const override {
        return frames_rendered_.load(std::memory_order_relaxed);
    }
    unsigned endpoint_frames() const override { return 480; }

private:
    void run();

    JitterBuffer& buffer_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::thread thread_;
};

} // namespace wfas
