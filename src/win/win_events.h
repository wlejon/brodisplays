#pragma once

#include <functional>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace brodisplays {

class WinDisplayWatcher {
public:
    explicit WinDisplayWatcher(std::function<void()> on_change);
    ~WinDisplayWatcher();

    WinDisplayWatcher(const WinDisplayWatcher&) = delete;
    WinDisplayWatcher& operator=(const WinDisplayWatcher&) = delete;

private:
    void thread_func();
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    std::function<void()> on_change_;
    std::thread thread_;
    HWND hwnd_ = nullptr;
    DWORD thread_id_ = 0;
    HANDLE ready_event_ = nullptr;
};

} // namespace brodisplays
