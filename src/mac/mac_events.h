#pragma once

#include <functional>

namespace brodisplays {

class MacDisplayWatcher {
public:
    explicit MacDisplayWatcher(std::function<void()> on_change);
    ~MacDisplayWatcher();

    MacDisplayWatcher(const MacDisplayWatcher&) = delete;
    MacDisplayWatcher& operator=(const MacDisplayWatcher&) = delete;

private:
    static void display_reconfig_callback(uint32_t display, uint32_t flags, void* user_info);

    std::function<void()> on_change_;
};

} // namespace brodisplays
