#pragma once

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
    Display* event_dpy_ = nullptr;
    std::mutex dpy_mutex_;

    std::function<void()> on_change_;
    std::thread watcher_thread_;
    bool watching_ = false;
    unsigned long dummy_window_ = 0;
};

} // namespace brodisplays
