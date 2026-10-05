#include "check.h"
#include "brodisplays/display_service.h"

#include <chrono>
#include <thread>

int main() {
    std::printf("[test_mac_revert] Starting macOS test-then-revert verification...\n");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto orig_snap = service->snapshot();
    const auto* primary = orig_snap.primary_display();
    if (!primary) {
        bstest::skip("test_mac_revert", "No display available to test mode change");
    }

    uint32_t orig_w = primary->current_mode.width;
    uint32_t orig_h = primary->current_mode.height;
    double orig_rr = primary->current_mode.refresh_rate;
    std::string disp_id = primary->id;

    std::printf("Original display %s mode: %ux%u @ %.1f Hz\n",
        disp_id.c_str(), orig_w, orig_h, orig_rr);

    brodisplays::DisplayConfigChange change;
    change.display_id = disp_id;

    bool found_alt = false;
    for (const auto& m : primary->available_modes) {
        if ((m.width != orig_w || m.height != orig_h) && m.width >= 1024 && m.height >= 768) {
            change.width = m.width;
            change.height = m.height;
            change.refresh_rate = m.refresh_rate;
            found_alt = true;
            break;
        }
    }

    if (!found_alt) {
        change.width = orig_w;
        change.height = orig_h;
        change.refresh_rate = orig_rr;
    }

    std::printf("Applying temporary mode: %ux%u with 1500ms revert timeout...\n",
        *change.width, *change.height);

    auto apply_res = service->apply_temporary_configuration(change, std::chrono::milliseconds(1500));
    if (!apply_res.ok) {
        std::printf("Note: apply_temporary_configuration returned: %s\n", apply_res.error.c_str());
        return bstest::finish("test_mac_revert");
    }

    CHECK(service->is_revert_pending());

    std::printf("Waiting for automatic revert...\n");
    bool reverted = bstest::wait_until([&] {
        return !service->is_revert_pending();
    }, std::chrono::milliseconds(3000));
    CHECK(reverted);
    CHECK(!service->is_revert_pending());

    auto restored_snap = service->snapshot();
    const auto* restored_primary = restored_snap.primary_display();
    REQUIRE(restored_primary != nullptr);

    CHECK_EQ(restored_primary->current_mode.width, orig_w);
    CHECK_EQ(restored_primary->current_mode.height, orig_h);

    return bstest::finish("test_mac_revert");
}
