#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "brodisplays/backlight.h"

namespace brodisplays {

class LinuxBacklightWatcher : public BacklightWatcher {
public:
    LinuxBacklightWatcher(
        std::string target_device_or_connector,
        std::function<void(const BrightnessEvent&)> on_event);

    ~LinuxBacklightWatcher() override;

    LinuxBacklightWatcher(const LinuxBacklightWatcher&) = delete;
    LinuxBacklightWatcher& operator=(const LinuxBacklightWatcher&) = delete;

    bool is_running() const noexcept override;
    void stop() override;

private:
    void run_loop();

    std::string target_;
    std::function<void(const BrightnessEvent&)> on_event_;
    std::atomic<bool> running_{false};
    int stop_event_fd_{-1};
    std::thread thread_;
};

} // namespace brodisplays
