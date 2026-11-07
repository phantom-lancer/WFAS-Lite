#include "wfas/jitter_buffer.hpp"
#include "wfas/wasapi_renderer.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool value, const char* name) {
    if (!value) { ++failures; std::cerr << "FAIL: " << name << "\n"; }
}
}

int main() {
    using namespace wfas;
    JitterBuffer buffer(20);

    {
        WasapiRenderer renderer(buffer);
        renderer.stop();
        renderer.stop();
        check(!renderer.running(), "stop before start is safe");

        // Forced startup failure: the render thread terminates on its own and
        // start()/stop() must join it reliably.  Repeat to exercise the
        // condition-variable handshake and repeated start/stop cycles.
        for (int i = 0; i < 20; ++i) {
            std::string error;
            renderer.fail_next_start_for_testing("forced startup failure");
            check(!renderer.start(error), "forced startup failure returns false");
            check(error == "forced startup failure", "forced startup failure preserves error");
            check(!renderer.running(), "failed start leaves renderer stopped");
            renderer.stop();
            renderer.stop();
        }

        // Real WASAPI start/stop/reuse.  A machine without a playback endpoint
        // is acceptable: the failure path is then validated instead.
        std::string error;
        const bool started = renderer.start(error);
        if (started) {
            std::cout << "Real WASAPI start/stop/reuse path exercised.\n";
            check(renderer.running(), "running after successful start");
            std::string second_error;
            check(renderer.start(second_error), "start while running is idempotent");
            check(second_error.empty(), "idempotent start has no error");
            renderer.stop();
            check(!renderer.running(), "stopped after stop");

            error.clear();
            check(renderer.start(error), "restart after stop succeeds");
            check(renderer.running(), "running after restart");
            renderer.stop();
            check(!renderer.running(), "stopped after second stop");
        } else {
            std::cout << "WASAPI unavailable; failure path exercised: " << error << "\n";
            check(!error.empty(), "real start failure reports an error");
            check(!renderer.running(), "failed real start leaves renderer stopped");
            renderer.stop();
        }
    }

    // Destructor must join a thread that was started but not explicitly stopped.
    {
        WasapiRenderer renderer(buffer);
        std::string error;
        renderer.start(error);
    }

    if (failures) return EXIT_FAILURE;
    std::cout << "All renderer lifecycle tests passed.\n";
    return 0;
}
