#include "check.h"
#include "linux/private_session.h"
#include "brodisplays/display_service.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>

// An X server that stops answering in the middle of a session (frozen,
// wedged, SIGSTOPped) must not hang DisplayService: every X11 request is
// bounded, the connection is abandoned once one times out, and the service
// falls back to the kernel's view. The server here is a private Xvfb, frozen
// with SIGSTOP after the service has connected and is watching it.

namespace {

using Clock = std::chrono::steady_clock;

long long ms_since(Clock::time_point t0) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

bool any_x11(const brodisplays::DisplaysSnapshot& snap) {
    for (const auto& d : snap.displays) {
        if (d.adapter_name == "X11 Display Device") return true;
    }
    return false;
}

} // namespace

int main() {
    const char* name = "test_linux_x11_stopped";
    std::printf("[%s] DisplayService when its X server stops answering mid-session\n", name);

    bdtest::TempDir dir("x11stop");
    REQUIRE(!dir.path().empty());
    bdtest::isolate_environment(dir);
    bdtest::Process xvfb = bdtest::start_xvfb(dir, 1280, 800);
    if (!xvfb.running()) bstest::skip(name, "Xvfb is not installed or would not start");

    brodisplays::DisplayServiceConfig config;
    config.enable_events = true;
    config.enable_gamma = true;
    std::string err;
    auto service = brodisplays::DisplayService::create(config, &err);
    REQUIRE(service != nullptr);
    auto before = service->snapshot();
    REQUIRE(any_x11(before));
    std::printf("Connected: %zu X11 display(s); primary %s\n", before.displays.size(),
                before.primary_display() ? before.primary_display()->id.c_str() : "(none)");

    REQUIRE(kill(xvfb.pid(), SIGSTOP) == 0);
    std::printf("Xvfb stopped\n");

    auto t0 = Clock::now();
    auto during = service->snapshot();
    long long first = ms_since(t0);
    std::printf("snapshot against the stopped server: %lld ms, %zu display(s)\n", first, during.displays.size());
    CHECK(first < 6000);
    CHECK(!any_x11(during));

    t0 = Clock::now();
    auto again = service->snapshot();
    long long second = ms_since(t0);
    std::printf("second snapshot (connection abandoned): %lld ms\n", second);
    CHECK(second < 1500);
    CHECK(!any_x11(again));

    brodisplays::DisplayConfigChange change;
    change.width = 1024;
    change.height = 768;
    t0 = Clock::now();
    auto res = service->apply_configuration(change);
    std::printf("apply_configuration: %lld ms: %s\n", ms_since(t0), res.error.c_str());
    CHECK(!res.ok);
    CHECK(ms_since(t0) < 3000);

    t0 = Clock::now();
    res = service->set_night_light(true, 3400);
    std::printf("set_night_light: %lld ms: %s\n", ms_since(t0), res.error.c_str());
    CHECK(!res.ok);
    CHECK(ms_since(t0) < 3000);

    // Draining what the watcher queued must not block either.
    service->events().drain();

    t0 = Clock::now();
    service.reset();
    std::printf("destroy: %lld ms\n", ms_since(t0));
    CHECK(ms_since(t0) < 3000);

    // A new service against the still-stopped server: the connection setup
    // is bounded too.
    t0 = Clock::now();
    auto late = brodisplays::DisplayService::create(config, &err);
    REQUIRE(late != nullptr);
    auto late_snap = late->snapshot();
    late.reset();
    std::printf("create + snapshot + destroy against the stopped server: %lld ms\n", ms_since(t0));
    CHECK(!any_x11(late_snap));
    CHECK(ms_since(t0) < 8000);

    kill(xvfb.pid(), SIGCONT);
    xvfb.stop();
    return bstest::finish(name);
}
