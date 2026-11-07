#pragma once

#include <cstdint>
#include <string>

namespace wfas {

struct ServerTarget {
    std::string ip;
    unsigned short port{9090};
    std::string hostname;
    bool from_discovery{false};

    bool valid() const { return !ip.empty() && port != 0; }
};

// Reconnection delays required for daily use: 1, 2, 4, 8, then 15 s maximum.
class ReconnectPolicy {
public:
    std::uint64_t next_delay_ms();
    void reset() { attempt_ = 0; }
    unsigned attempts() const { return attempt_; }

private:
    unsigned attempt_{};
};

// %LOCALAPPDATA%\WFAS-Lite on Windows, $XDG_STATE_HOME/wfas-lite (or
// ~/.local/state/wfas-lite) elsewhere.  The directory is created if needed.
std::string default_state_dir();

// The last successfully connected server is stored as plain text and used as
// the fallback when multicast discovery finds nothing.
bool save_last_server(const std::string& path, const ServerTarget& target,
                      std::string& error);
bool load_last_server(const std::string& path, ServerTarget& target,
                      std::string& error);

} // namespace wfas
