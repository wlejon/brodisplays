#include "check.h"
#include "brodisplays/display_service.h"

#include <chrono>
#include <thread>

int main() {
    std::printf("[test_win_events] Starting Windows event watcher test...\n");

    brodisplays::DisplayServiceConfig cfg;
    cfg.enable_events = true;

    std::string err;
    auto service = brodisplays::DisplayService::create(cfg, &err);
    REQUIRE(service != nullptr);

    // Initial snapshot should be immediately available
    auto snap = service->snapshot();
    CHECK(!snap.displays.empty());

    // Event queue should be accessible
    auto& eq = service->events();
    CHECK_EQ(eq.size(), 0u);

    // Test wait_for timeout on event queue
    bool wait_res = eq.wait_for(std::chrono::milliseconds(50));
    CHECK(!wait_res);

    return bstest::finish("test_win_events");
}
