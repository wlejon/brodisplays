#include "check.h"
#include "brodisplays/display_service.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <windows.h>

// The watcher has to receive the system's display broadcasts. Windows sends
// WM_DISPLAYCHANGE / WM_SETTINGCHANGE to top-level windows only, which are
// exactly the windows EnumWindows lists: finding the watcher's window there
// proves a monitor hot-plug reaches it (a message-only window would not be
// listed and would never hear about one). The messages are then delivered to
// that window alone, which changes nothing on the desktop.

namespace {

HWND find_watcher_window() {
    struct Find {
        DWORD pid;
        HWND found;
    } f{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            wchar_t cls[128] = {};
            GetClassNameW(h, cls, 128);
            if (pid == f->pid && std::wstring(cls) == L"BroDisplaysWatcherWindowClass") {
                f->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

size_t count_changes(const std::vector<brodisplays::DisplayEvent>& events) {
    size_t n = 0;
    for (const auto& e : events) n += std::holds_alternative<brodisplays::DisplaysChanged>(e) ? 1 : 0;
    return n;
}

} // namespace

int main() {
    std::printf("[test_win_events] Starting Windows event watcher test...\n");

    // Without events there is no watcher window.
    {
        brodisplays::DisplayServiceConfig quiet;
        quiet.enable_events = false;
        auto service = brodisplays::DisplayService::create(quiet);
        REQUIRE(service != nullptr);
        CHECK(find_watcher_window() == nullptr);
    }

    brodisplays::DisplayServiceConfig cfg;
    cfg.enable_events = true;

    std::string err;
    auto service = brodisplays::DisplayService::create(cfg, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    CHECK(!snap.displays.empty());

    auto& eq = service->events();
    CHECK_EQ(eq.size(), 0u);
    CHECK(!eq.wait_for(std::chrono::milliseconds(50)));

    HWND watcher = find_watcher_window();
    REQUIRE(watcher != nullptr);
    CHECK(GetAncestor(watcher, GA_PARENT) == GetDesktopWindow());
    CHECK(!IsWindowVisible(watcher));
    CHECK((GetWindowLongPtrW(watcher, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0);

    // A display change is published with the current displays.
    std::atomic<int> woken{0};
    eq.set_wake([&] { ++woken; });
    SendMessageW(watcher, WM_DISPLAYCHANGE, 32, MAKELPARAM(snap.displays[0].current_mode.width,
                                                           snap.displays[0].current_mode.height));
    REQUIRE(eq.wait_for(std::chrono::milliseconds(2000)));
    auto events = eq.drain();
    CHECK_EQ(count_changes(events), 1u);
    CHECK(woken.load() >= 1);
    for (const auto& e : events) {
        if (auto* c = std::get_if<brodisplays::DisplaysChanged>(&e)) CHECK(c->snapshot.same_displays(snap));
    }

    // A settings broadcast that leaves the displays as they are is not a
    // display change.
    SendMessageW(watcher, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"));
    SendMessageW(watcher, WM_DPICHANGED, MAKEWPARAM(96, 96), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(count_changes(eq.drain()), 0u);
    eq.set_wake(nullptr);

    // Two services in one process share the window class; both watch.
    {
        auto second = brodisplays::DisplayService::create(cfg);
        REQUIRE(second != nullptr);
        CHECK(second->snapshot().same_displays(snap));
    }
    CHECK(find_watcher_window() != nullptr);

    service.reset();
    CHECK(find_watcher_window() == nullptr);

    return bstest::finish("test_win_events");
}
