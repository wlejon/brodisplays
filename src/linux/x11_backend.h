#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "brodisplays/types.h"

// Forward-declare X11 / XRandR types so consumers don't need X11 headers
typedef struct _XDisplay Display;

namespace brodisplays {

class X11Backend {
public:
    static bool is_available();
    static std::unique_ptr<X11Backend> create(std::string* error = nullptr);

    ~X11Backend();

    std::vector<DisplayInfo> snapshot();
    Result apply_configuration(const DisplayConfigChange& change);

    void start_watcher(std::function<void()> on_change);
    void stop_watcher();

private:
    X11Backend(Display* dpy);
    void watcher_thread_func();

    Display* dpy_ = nullptr;
    // Owned by the watcher thread between start_watcher() and the join in
    // stop_watcher(); no other thread touches it in that window.
    Display* event_dpy_ = nullptr;
    std::mutex dpy_mutex_;

    std::function<void()> on_change_;
    std::thread watcher_thread_;
    std::atomic<bool> watching_{false};
    int wake_pipe_[2] = {-1, -1};
};

} // namespace brodisplays
