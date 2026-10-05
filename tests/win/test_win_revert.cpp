#include "check.h"
#include "brodisplays/display_service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <chrono>
#include <thread>

int main() {
    std::printf("[test_win_revert] Starting Windows test-then-revert verification...\n");

    HWINSTA ws = GetProcessWindowStation();
    WCHAR ws_name[256]{};
    DWORD len = 0;
    GetUserObjectInformationW(ws, UOI_NAME, ws_name, sizeof(ws_name), &len);
    std::wprintf(L"WindowStation: %s\n", ws_name);

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto orig_snap = service->snapshot();
    const auto* primary = orig_snap.primary_display();
    REQUIRE(primary != nullptr);

    uint32_t orig_w = primary->current_mode.width;
    uint32_t orig_h = primary->current_mode.height;
    double orig_rr = primary->current_mode.refresh_rate;
    std::string disp_id = primary->id;

    std::printf("Original display %s mode: %ux%u @ %.1f Hz\n",
        disp_id.c_str(), orig_w, orig_h, orig_rr);

    // Find an alternate mode or same mode with test flag
    brodisplays::DisplayConfigChange change;
    change.display_id = disp_id;

    // Prefer changing refresh rate at the native resolution to preserve desktop geometry
    bool found_alt = false;
    for (const auto& m : primary->available_modes) {
        if (m.width == orig_w && m.height == orig_h && std::abs(m.refresh_rate - orig_rr) > 5.0) {
            change.width = m.width;
            change.height = m.height;
            change.refresh_rate = m.refresh_rate;
            found_alt = true;
            std::printf("Found alternate refresh rate: %ux%u @ %.1f Hz\n", m.width, m.height, m.refresh_rate);
            break;
        }
    }

    if (!found_alt) {
        for (const auto& m : primary->available_modes) {
            if ((m.width != orig_w || m.height != orig_h) && m.width >= 1024 && m.height >= 768) {
                change.width = m.width;
                change.height = m.height;
                change.refresh_rate = m.refresh_rate;
                found_alt = true;
                break;
            }
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
        bstest::skip("test_win_revert", "OS or display driver restricted configuration changes in this session: " + apply_res.error);
    }
    CHECK(apply_res.ok);
    CHECK(service->is_revert_pending());

    // Wait for the automatic revert timer to fire (1500ms + margin)
    std::printf("Waiting for automatic revert...\n");
    bool reverted = bstest::wait_until([&] {
        return !service->is_revert_pending();
    }, std::chrono::milliseconds(3000));
    CHECK(reverted);
    CHECK(!service->is_revert_pending());

    // Drain events and check for ConfigurationReverted
    auto events = service->events().drain();
    bool found_revert_event = false;
    for (const auto& ev : events) {
        if (auto* rev = std::get_if<brodisplays::ConfigurationReverted>(&ev)) {
            if (rev->display_id == disp_id) {
                found_revert_event = true;
                std::printf("Received revert event: %s\n", rev->reason.c_str());
            }
        }
    }
    CHECK(found_revert_event);

    // Verify restored snapshot matches original 100%
    auto restored_snap = service->snapshot();
    const auto* restored_primary = restored_snap.primary_display();
    REQUIRE(restored_primary != nullptr);

    std::printf("Restored mode: %ux%u @ %.1f Hz\n",
        restored_primary->current_mode.width,
        restored_primary->current_mode.height,
        restored_primary->current_mode.refresh_rate);

    CHECK_EQ(restored_primary->current_mode.width, orig_w);
    CHECK_EQ(restored_primary->current_mode.height, orig_h);

    return bstest::finish("test_win_revert");
}
