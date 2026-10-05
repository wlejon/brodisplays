#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "brodisplays/types.h"
#include "x11_connection.h"

namespace brodisplays {

// X11 display backend: RandR 1.3+ over XCB. Every exchange with the server
// runs under the connection's watchdog (X11Connection::kIoTimeout): a server
// that stops answering, even one that was fine when the service started, is
// abandoned and the backend reports itself broken (snapshot() then returns
// nothing, so the service falls back to the kernel's view) instead of
// blocking the caller. The watcher has a connection of its own and never
// blocks inside XCB.
class X11Backend {
public:
    // nullptr when $DISPLAY names no X server that answers, or one without
    // RandR 1.3.
    static std::unique_ptr<X11Backend> create(std::string* error = nullptr);

    ~X11Backend();

    std::vector<DisplayInfo> snapshot();
    Result apply_configuration(const DisplayConfigChange& change);

    // Night light by CRTC gamma ramps (the RandR equivalent of
    // wlr-gamma-control): every active output's ramp is the one it had before
    // the first change, scaled by the black-body gains of the temperature.
    // Restored when disabled and when the backend is destroyed.
    Result set_night_light(bool enabled, uint32_t temperature_kelvin);
    // Whether any active output has a gamma ramp (RandR gamma size > 0).
    bool gamma_supported();

    void start_watcher(std::function<void()> on_change);
    void stop_watcher();

    bool broken() const;

private:
    explicit X11Backend(std::unique_ptr<X11Connection> conn);
    void watcher_thread_func();
    void restore_gamma_locked();

    std::unique_ptr<X11Connection> conn_;
    std::mutex mutex_;

    // Gamma state (guarded by mutex_): the original ramp of each CRTC we
    // changed, and the night light the outputs show.
    struct SavedGamma {
        std::vector<uint16_t> red, green, blue;
    };
    std::map<uint32_t, SavedGamma> saved_gamma_;
    bool night_light_on_ = false;
    uint32_t night_light_kelvin_ = 6500;

    // Owned by the watcher thread between start_watcher() and the join in
    // stop_watcher(); no other thread touches it in that window.
    std::unique_ptr<X11Connection> event_conn_;
    uint8_t rr_first_event_ = 0;
    std::function<void()> on_change_;
    std::thread watcher_thread_;
    std::atomic<bool> watching_{false};
    int wake_pipe_[2] = {-1, -1};
};

} // namespace brodisplays
