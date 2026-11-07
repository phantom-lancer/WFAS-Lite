#include "wfas/wasapi_renderer.hpp"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <chrono>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

namespace wfas {
WasapiRenderer::~WasapiRenderer() { stop(); }

#ifdef WFAS_LITE_TESTING
void WasapiRenderer::fail_next_start_for_testing(std::string reason) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    forced_startup_failure_ = std::move(reason);
}
#endif

void WasapiRenderer::complete_startup(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        startup_error_ = error;
        startup_complete_ = true;
    }
    if (!error.empty()) running_.store(false, std::memory_order_release);
    startup_cv_.notify_all();
}

bool WasapiRenderer::start(std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);

    if (thread_.joinable()) {
        if (running_.load(std::memory_order_acquire)) {
            return true; // already running; start is idempotent
        }
        thread_.join(); // reap a thread that terminated on its own
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        startup_complete_ = false;
        startup_error_.clear();
    }
    frames_rendered_.store(0, std::memory_order_relaxed);
    endpoint_frames_.store(0, std::memory_order_relaxed);

#ifdef WFAS_LITE_TESTING
    const std::string forced_failure = std::move(forced_startup_failure_);
    forced_startup_failure_.clear();
#endif

    running_.store(true, std::memory_order_release);
    try {
#ifdef WFAS_LITE_TESTING
        thread_ = std::thread([this, forced_failure] {
            if (!forced_failure.empty()) {
                complete_startup(forced_failure);
                return;
            }
            run();
        });
#else
        thread_ = std::thread(&WasapiRenderer::run, this);
#endif
    } catch (const std::system_error& ex) {
        running_.store(false, std::memory_order_release);
        error = std::string("Cannot start WASAPI render thread: ") + ex.what();
        return false;
    }

    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        completed = startup_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return startup_complete_ || !running_.load(std::memory_order_acquire);
        });
        if (startup_complete_) error = startup_error_;
    }

    if (!completed) {
        error = "WASAPI renderer startup timed out";
    } else if (error.empty() && !startup_complete_) {
        error = "WASAPI renderer thread exited before startup completed";
    }

    if (!error.empty()) {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void WasapiRenderer::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void WasapiRenderer::run() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { complete_startup("CoInitializeEx failed"); return; }
    IMMDeviceEnumerator* enumerator = nullptr; IMMDevice* device = nullptr;
    IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr; HANDLE event = nullptr;
    auto cleanup = [&] {
        if (client) client->Stop();
        if (event) CloseHandle(event);
        if (render) render->Release(); if (client) client->Release();
        if (device) device->Release(); if (enumerator) enumerator->Release();
        CoUninitialize();
    };
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) { complete_startup("Cannot create Windows audio device enumerator"); cleanup(); return; }
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) { complete_startup("No default Windows playback endpoint"); cleanup(); return; }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (FAILED(hr)) { complete_startup("Cannot open default playback endpoint"); cleanup(); return; }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &format, nullptr);
    if (FAILED(hr)) { complete_startup("WASAPI shared-mode initialization failed (0x" + std::to_string(static_cast<unsigned long>(hr)) + ")"); cleanup(); return; }
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event || FAILED(client->SetEventHandle(event))) { complete_startup("Cannot create WASAPI render event"); cleanup(); return; }
    UINT32 frames = 0;
    hr = client->GetBufferSize(&frames);
    // The render buffer is always expressed in frames of the client format
    // requested above (48 kHz stereo S16LE, 4 bytes per frame).  Reject
    // anything implausible instead of multiplying an unchecked size.
    constexpr UINT32 kMaxEndpointFrames = 48000u * 4u; // 4 s of audio
    if (FAILED(hr) || frames == 0 || frames > kMaxEndpointFrames) {
        complete_startup("WASAPI returned an invalid endpoint buffer size");
        cleanup();
        return;
    }
    endpoint_frames_.store(frames, std::memory_order_relaxed);
    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    if (FAILED(hr)) { complete_startup("Cannot acquire WASAPI render service"); cleanup(); return; }
    // Pre-fill silence so the endpoint never starts with stale audio.
    BYTE* initial = nullptr;
    if (SUCCEEDED(render->GetBuffer(frames, &initial)) && initial) {
        std::memset(initial, 0, static_cast<std::size_t>(frames) * kStereoFrameBytes);
        render->ReleaseBuffer(frames, 0);
    }
    hr = client->Start();
    if (FAILED(hr)) { complete_startup("Cannot start WASAPI renderer"); cleanup(); return; }
    complete_startup({});
    while (running_.load(std::memory_order_acquire)) {
        const DWORD wait = WaitForSingleObject(event, 250);
        if (wait != WAIT_OBJECT_0) continue;
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding)) || padding > frames) continue;
        const UINT32 available = frames - padding;
        if (!available) continue;
        BYTE* out = nullptr;
        if (SUCCEEDED(render->GetBuffer(available, &out)) && out) {
            const std::size_t out_bytes =
                static_cast<std::size_t>(available) * kStereoFrameBytes;
            buffer_.read(out, out_bytes);
            if (SUCCEEDED(render->ReleaseBuffer(available, 0))) {
                frames_rendered_.fetch_add(available, std::memory_order_relaxed);
            }
        }
    }
    cleanup();
}
} // namespace wfas
