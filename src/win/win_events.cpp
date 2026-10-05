#include "win_events.h"

namespace brodisplays {

WinDisplayWatcher::WinDisplayWatcher(std::function<void()> on_change)
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

    const wchar_t* kClassName = L"BroDisplaysWatcherWindowClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WinDisplayWatcher::window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        0, kClassName, L"BroDisplaysWatcher", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, this);

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
        case WM_SETTINGCHANGE: {
            if (watcher->on_change_) {
                watcher->on_change_();
            }
            return 0;
        }
        case WM_CLOSE: {
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace brodisplays
