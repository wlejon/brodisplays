#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace brodisplays {

// Reports display reconfigurations. CoreGraphics delivers its reconfiguration
// callback through the main thread's run loop, so in a process that does not
// run one (a daemon, a test, a host with its own loop) it never fires. A
// background thread therefore also compares a cheap fingerprint of the
// display set (ids, bounds, modes, sleep, mirroring) every kPollInterval;
// CoreGraphics answers those queries from any thread. on_change may be called
// for no actual change; the service filters by comparing snapshots.
class MacDisplayWatcher {
public:
    static constexpr std::chrono::milliseconds kPollInterval{500};

    explicit MacDisplayWatcher(std::function<void()> on_change);
    ~MacDisplayWatcher();

    MacDisplayWatcher(const MacDisplayWatcher&) = delete;
    MacDisplayWatcher& operator=(const MacDisplayWatcher&) = delete;

    // The fingerprint the poller compares (exposed for tests).
    static std::string fingerprint();

private:
    static void display_reconfig_callback(uint32_t display, uint32_t flags, void* user_info);
    void poll_loop();

    std::function<void()> on_change_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread poller_;
};

} // namespace brodisplays
