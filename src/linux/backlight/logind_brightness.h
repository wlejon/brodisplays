#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "brodisplays/types.h"

namespace brodisplays {

class LogindBrightnessClient {
public:
    LogindBrightnessClient();
    ~LogindBrightnessClient();

    // Non-copyable, movable
    LogindBrightnessClient(const LogindBrightnessClient&) = delete;
    LogindBrightnessClient& operator=(const LogindBrightnessClient&) = delete;
    LogindBrightnessClient(LogindBrightnessClient&&) noexcept;
    LogindBrightnessClient& operator=(LogindBrightnessClient&&) noexcept;

    // Checks whether connection to systemd-logind over D-Bus is available
    bool is_available() const noexcept;

    // Sets device brightness via org.freedesktop.login1.Session.SetBrightness
    Result set_brightness(std::string_view device_name, uint32_t raw_brightness);

    // Resolves current session object path (e.g. "/org/freedesktop/login1/session/_34")
    std::string resolve_session_path();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace brodisplays
