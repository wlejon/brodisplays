#pragma once

#include <functional>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace brodisplays {

// Watches for display changes on a thread of its own. The window is a hidden
// top-level window, not a message-only one: WM_DISPLAYCHANGE and
// WM_SETTINGCHANGE are broadcast to top-level windows only, so a
// message-only window never sees a monitor being plugged in.
//
// on_change(definite): definite is true for WM_DISPLAYCHANGE (the display
// configuration did change); false for the hints that may or may not concern
// displays (WM_SETTINGCHANGE, WM_DPICHANGED, device arrival), which the
// service filters by comparing snapshots.
class WinDisplayWatcher {
public:
    static constexpr const wchar_t* kClassName = L"BroDisplaysWatcherWindowClass";

    explicit WinDisplayWatcher(std::function<void(bool definite)> on_change);
    ~WinDisplayWatcher();

    WinDisplayWatcher(const WinDisplayWatcher&) = delete;
    WinDisplayWatcher& operator=(const WinDisplayWatcher&) = delete;

private:
    void thread_func();
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    std::function<void(bool)> on_change_;
    std::thread thread_;
    HWND hwnd_ = nullptr;
    DWORD thread_id_ = 0;
    HANDLE ready_event_ = nullptr;
};

// Runs the enclosed scope with a per-monitor-v2 DPI context, so geometry and
// GetDpiForMonitor answer in physical pixels / the real effective DPI
// whatever the host process's own DPI awareness is.
class DpiScope {
public:
    DpiScope();
    ~DpiScope();
    DpiScope(const DpiScope&) = delete;
    DpiScope& operator=(const DpiScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT old_ = nullptr;
};

} // namespace brodisplays
