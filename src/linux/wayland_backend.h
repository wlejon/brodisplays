#pragma once

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

class WaylandBackend {
public:
    static bool is_available();
    static std::unique_ptr<WaylandBackend> create(std::string* error = nullptr);

    ~WaylandBackend();

    std::vector<DisplayInfo> snapshot();
    Result apply_configuration(const DisplayConfigChange& change);

    void start_watcher(std::function<void()> on_change);
    void stop_watcher();

    struct HeadData;
    struct OutputData;

private:
    WaylandBackend(wl_display* display);
    void event_loop_func();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    zwlr_output_manager_v1* wlr_manager_ = nullptr;
    uint32_t wlr_serial_ = 0;

    std::recursive_mutex state_mutex_;
    std::vector<OutputData*> outputs_;
    std::vector<HeadData*> heads_;

    std::function<void()> on_change_;
    std::thread loop_thread_;
    bool running_ = false;
    int wake_pipe_[2] = {-1, -1};

    friend struct WaylandListeners;
};

} // namespace brodisplays
