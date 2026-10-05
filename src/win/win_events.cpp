#include "win_events.h"

namespace brodisplays {

DpiScope::DpiScope() {
    old_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

DpiScope::~DpiScope() {
    if (old_) SetThreadDpiAwarenessContext(old_);
}

WinDisplayWatcher::WinDisplayWatcher(std::function<void(bool)> on_change)
    : on_change_(std::move(on_change)) {
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread(&WinDisplayWatcher::thread_func, this);
    if (ready_event_) {
        WaitForSingleObject(ready_event_, INFINITE);
    }
}

WinDisplayWatcher::~WinDisplayWatcher() {
    if (hwnd_) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else if (thread_id_ != 0) {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (ready_event_) {
        CloseHandle(ready_event_);
    }
}

void WinDisplayWatcher::thread_func() {
    thread_id_ = GetCurrentThreadId();
    // The snapshots taken from this thread see physical pixels and real DPI.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WinDisplayWatcher::window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    // Several services in one process share the class; a second
    // registration fails harmlessly with ERROR_CLASS_ALREADY_EXISTS.
    RegisterClassExW(&wc);

    // Hidden, never activated, absent from the taskbar and Alt+Tab.
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"BroDisplaysWatcher", WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, wc.hInstance, this);

    if (ready_event_) {
        SetEvent(ready_event_);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    // Fails while another service's window still uses the class; fine.
    UnregisterClassW(kClassName, wc.hInstance);
}

LRESULT CALLBACK WinDisplayWatcher::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    auto* watcher = reinterpret_cast<WinDisplayWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!watcher) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
        case WM_DISPLAYCHANGE:
            if (watcher->on_change_) watcher->on_change_(true);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DPICHANGED:
        case WM_DEVICECHANGE:
            if (watcher->on_change_) watcher->on_change_(false);
            if (msg == WM_DEVICECHANGE) return TRUE;
            return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace brodisplays
