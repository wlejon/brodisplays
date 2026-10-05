#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "brodisplays/types.h"

// Forward-declare Wayland types
struct wl_display;
struct wl_registry;
struct wl_output;
struct zwlr_output_manager_v1;

namespace brodisplays {

// Wayland display backend.
//
// Enumeration uses zwlr_output_management_v1 when the compositor offers it and
// falls back to plain wl_output otherwise; mode changes require the wlr
// protocol and fail honestly without it. Every exchange with the compositor
// is bounded by kIoTimeout: a compositor that stops answering marks the
// backend broken (snapshot() then returns nothing so the service falls back)
// instead of blocking the caller.
class WaylandBackend {
public:
    static constexpr std::chrono::milliseconds kIoTimeout{2000};

    static std::unique_ptr<WaylandBackend> create(std::string* error = nullptr);

    ~WaylandBackend();

    std::vector<DisplayInfo> snapshot();
    Result apply_configuration(const DisplayConfigChange& change);

    // True when the compositor offers zwlr_output_management_v1.
    bool can_configure();

    void start_watcher(std::function<void()> on_change);
    void stop_watcher();

    struct HeadData;
    struct OutputData;
    struct ModeItem;
    struct Listeners;

private:
    explicit WaylandBackend(wl_display* display);

    bool initialize(std::string* error);
    // Dispatches compositor events until done() holds, the deadline passes, or
    // the connection fails. Caller holds state_mutex_. Works with or without
    // the watcher thread (which then does the reading).
    bool dispatch_until(std::unique_lock<std::recursive_mutex>& lock,
                        const std::function<bool()>& done,
                        std::chrono::milliseconds timeout);
    bool roundtrip(std::unique_lock<std::recursive_mutex>& lock);
    void mark_broken(const char* why);
    void notify_change();
    void event_loop_func();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    zwlr_output_manager_v1* wlr_manager_ = nullptr;
    uint32_t wlr_version_ = 0;
    uint32_t wlr_serial_ = 0;
    uint32_t wlr_done_count_ = 0;

    std::recursive_mutex state_mutex_;
    std::vector<OutputData*> outputs_;
    std::vector<HeadData*> heads_;

    std::function<void()> on_change_;
    std::thread loop_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> broken_{false};
    int wake_pipe_[2] = {-1, -1};
};

} // namespace brodisplays
