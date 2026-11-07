#include "wfas/reconnect.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace wfas {
namespace {

constexpr std::uint64_t kDelaysMs[] = {1000, 2000, 4000, 8000, 15000};

bool parse_port(const std::string& text, unsigned short& out) {
    if (text.empty()) return false;
    unsigned long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned long>(c - '0');
        if (value > 65535) return false;
    }
    if (value == 0) return false;
    out = static_cast<unsigned short>(value);
    return true;
}

bool parse_ipv4_literal(const std::string& text) {
    int parts = 0;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = text.find('.', start);
        const std::string octet =
            dot == std::string::npos ? text.substr(start)
                                     : text.substr(start, dot - start);
        if (octet.empty() || octet.size() > 3) return false;
        unsigned value = 0;
        for (const char c : octet) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value > 255) return false;
        ++parts;
        if (dot == std::string::npos) break;
        start = dot + 1;
        if (parts > 4) return false;
    }
    return parts == 4;
}

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && static_cast<unsigned char>(value[begin]) <= 0x20) ++begin;
    while (end > begin && static_cast<unsigned char>(value[end - 1]) <= 0x20) --end;
    return value.substr(begin, end - begin);
}

} // namespace

std::uint64_t ReconnectPolicy::next_delay_ms() {
    constexpr std::size_t count = sizeof(kDelaysMs) / sizeof(kDelaysMs[0]);
    const std::size_t index = std::min<std::size_t>(attempt_, count - 1);
    ++attempt_;
    return kDelaysMs[index];
}

std::string default_state_dir() {
    std::filesystem::path directory;
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) base = std::getenv("APPDATA");
    if (!base || !*base) base = std::getenv("USERPROFILE");
    directory = (base && *base) ? std::filesystem::path(base)
                                : std::filesystem::path(".");
    directory /= "WFAS-Lite";
#else
    const char* xdg = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) {
        directory = std::filesystem::path(xdg) / "wfas-lite";
    } else if (home && *home) {
        directory = std::filesystem::path(home) / ".local" / "state" / "wfas-lite";
    } else {
        directory = std::filesystem::path(".");
    }
#endif
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory.string();
}

bool save_last_server(const std::string& path, const ServerTarget& target,
                      std::string& error) {
    error.clear();
    if (!target.valid() || !parse_ipv4_literal(target.ip)) {
        error = "refusing to save an invalid server target";
        return false;
    }
    const std::filesystem::path destination(path);
    std::error_code directory_error;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(),
                                            directory_error);
    }
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write " + temporary.string();
            return false;
        }
        file << "# WFAS-Lite last successful server\n";
        file << "ip=" << target.ip << "\n";
        file << "port=" << target.port << "\n";
        if (!target.hostname.empty()) file << "hostname=" << target.hostname << "\n";
        file.flush();
        if (!file) {
            error = "cannot write " + temporary.string();
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, destination, rename_error);
    if (rename_error) {
        std::filesystem::remove(destination, rename_error);
        rename_error.clear();
        std::filesystem::rename(temporary, destination, rename_error);
        if (rename_error) {
            error = "cannot replace " + destination.string();
            return false;
        }
    }
    return true;
}

bool load_last_server(const std::string& path, ServerTarget& target,
                      std::string& error) {
    error.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "no saved server";
        return false;
    }
    ServerTarget parsed;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "ip") {
            parsed.ip = value;
        } else if (key == "port") {
            if (!parse_port(value, parsed.port)) {
                error = "saved server has an invalid port";
                return false;
            }
        } else if (key == "hostname") {
            parsed.hostname = value;
        }
    }
    if (!parsed.valid() || !parse_ipv4_literal(parsed.ip)) {
        error = "saved server is incomplete or has an invalid address";
        return false;
    }
    parsed.from_discovery = false;
    target = parsed;
    return true;
}

} // namespace wfas
