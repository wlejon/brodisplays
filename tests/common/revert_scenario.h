#pragma once

// Shared body of the per-platform test-then-revert tests: switch the primary
// display to another mode with a short revert timeout, let the timer expire,
// and verify the ConfigurationReverted event and the restored mode.
// Opt-in only (BRODISPLAYS_TEST_MUTATE=1): it visibly changes the display.

#include "check.h"
#include "brodisplays/display_service.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bdtest {

inline int run_revert_scenario(const char* name) {
    bstest::require_mutation_opt_in(name, "test-then-revert switches the primary display mode and");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto orig_snap = service->snapshot();
    const auto* primary = orig_snap.primary_display();
    if (!primary) bstest::skip(name, "no display available to change");

    const brodisplays::DisplayMode orig = primary->current_mode;
    const std::string disp_id = primary->id;
    std::printf("Original display %s mode: %ux%u @ %.2f Hz\n",
                disp_id.c_str(), orig.width, orig.height, orig.refresh_rate);

    // Prefer another refresh rate at the current resolution (desktop geometry
    // untouched), else another resolution of at least 1024x768.
    const brodisplays::DisplayMode* alt = nullptr;
    for (const auto& m : primary->available_modes) {
        if (m.width == orig.width && m.height == orig.height &&
            std::abs(m.refresh_rate - orig.refresh_rate) > 5.0) {
            alt = &m;
            break;
        }
    }
    for (const auto& m : primary->available_modes) {
        if (alt) break;
        if ((m.width != orig.width || m.height != orig.height) && m.width >= 1024 && m.height >= 768) {
            alt = &m;
        }
    }
    if (!alt) bstest::skip(name, "the primary display offers no alternate mode");

    brodisplays::DisplayConfigChange change;
    change.display_id = disp_id;
    change.width = alt->width;
    change.height = alt->height;
    change.refresh_rate = alt->refresh_rate;
    std::printf("Applying temporary mode %ux%u @ %.2f Hz with a 1500 ms revert timeout...\n",
                alt->width, alt->height, alt->refresh_rate);

    auto apply_res = service->apply_temporary_configuration(change, std::chrono::milliseconds(1500));
    if (!apply_res.ok) {
        bstest::skip(name, "the platform refused the mode change: " + apply_res.error);
    }
    CHECK(service->is_revert_pending());

    auto during = service->snapshot();
    if (const auto* p = during.find_display(disp_id)) {
        std::printf("Temporary mode: %ux%u @ %.2f Hz\n",
                    p->current_mode.width, p->current_mode.height, p->current_mode.refresh_rate);
    }

    // Once the configuration stops being pending, the rollback has run and its
    // event is queued; nothing here waits for a settle period on purpose.
    bool reverted = bstest::wait_until([&] { return !service->is_revert_pending(); },
                                       std::chrono::milliseconds(5000));
    CHECK(reverted);

    // The watcher saw both changes (to the test mode and back): a
    // DisplaysChanged showing the test mode, and later one showing the
    // original. It runs without a main run loop here, so on macOS this is the
    // poller's doing.
    std::vector<brodisplays::DisplayEvent> seen;
    // Index of the first DisplaysChanged at or after `from` showing `want`.
    auto saw_mode = [&](const brodisplays::DisplayMode& want, size_t from) -> size_t {
        auto more = service->events().drain();
        seen.insert(seen.end(), more.begin(), more.end());
        for (size_t i = from; i < seen.size(); ++i) {
            if (auto* c = std::get_if<brodisplays::DisplaysChanged>(&seen[i])) {
                if (const auto* p = c->snapshot.find_display(disp_id)) {
                    if (p->current_mode.width == want.width && p->current_mode.height == want.height &&
                        std::abs(p->current_mode.refresh_rate - want.refresh_rate) < 1.0) return i;
                }
            }
        }
        return SIZE_MAX;
    };
    size_t at_alt = SIZE_MAX, at_orig = SIZE_MAX;
    CHECK(bstest::wait_until([&] { return (at_alt = saw_mode(*alt, 0)) != SIZE_MAX; },
                             std::chrono::milliseconds(3000)));
    if (at_alt != SIZE_MAX) {
        CHECK(bstest::wait_until([&] { return (at_orig = saw_mode(orig, at_alt + 1)) != SIZE_MAX; },
                                 std::chrono::milliseconds(3000)));
    }
    std::printf("DisplaysChanged: test mode %s, original mode %s\n", at_alt != SIZE_MAX ? "seen" : "MISSING",
                at_orig != SIZE_MAX ? "seen" : "MISSING");

    bool found_revert_event = false;
    for (const auto& ev : seen) {
        if (auto* rev = std::get_if<brodisplays::ConfigurationReverted>(&ev)) {
            if (rev->display_id == disp_id) {
                found_revert_event = true;
                std::printf("Revert event: %s (restored=%d%s%s)\n", rev->reason.c_str(), rev->restored,
                            rev->error.empty() ? "" : ", error: ", rev->error.c_str());
                CHECK(rev->restored);
            }
        }
    }
    CHECK(found_revert_event);

    auto restored_snap = service->snapshot();
    const auto* restored = restored_snap.find_display(disp_id);
    REQUIRE(restored != nullptr);
    std::printf("Restored mode: %ux%u @ %.2f Hz\n", restored->current_mode.width,
                restored->current_mode.height, restored->current_mode.refresh_rate);
    CHECK_EQ(restored->current_mode.width, orig.width);
    CHECK_EQ(restored->current_mode.height, orig.height);
    CHECK(std::abs(restored->current_mode.refresh_rate - orig.refresh_rate) < 1.0);

    // Phase 2: a resolution change (desktop geometry changes too), undone by
    // an explicit revert_configuration() long before its timer.
    const brodisplays::DisplayMode* res_alt = nullptr;
    for (const auto& m : primary->available_modes) {
        if ((m.width != orig.width || m.height != orig.height) && m.width >= 1024 && m.height >= 768) {
            res_alt = &m;
            break;
        }
    }
    if (res_alt) {
        brodisplays::DisplayConfigChange c2;
        c2.display_id = disp_id;
        c2.width = res_alt->width;
        c2.height = res_alt->height;
        c2.refresh_rate = res_alt->refresh_rate;
        std::printf("Applying temporary resolution %ux%u @ %.2f Hz, then reverting explicitly...\n",
                    res_alt->width, res_alt->height, res_alt->refresh_rate);
        auto r2 = service->apply_temporary_configuration(c2, std::chrono::seconds(30));
        if (!r2.ok) {
            std::printf("Note: resolution change refused: %s\n", r2.error.c_str());
        } else {
            auto mid = service->snapshot();
            if (const auto* p = mid.find_display(disp_id)) {
                CHECK_EQ(p->current_mode.width, res_alt->width);
                CHECK_EQ(p->current_mode.height, res_alt->height);
            }
            auto rv = service->revert_configuration();
            if (!rv.ok) std::printf("revert_configuration failed: %s\n", rv.error.c_str());
            CHECK(rv.ok);
            CHECK(!service->is_revert_pending());
            auto back = service->snapshot();
            const auto* b = back.find_display(disp_id);
            REQUIRE(b != nullptr);
            std::printf("After explicit revert: %ux%u @ %.2f Hz\n", b->current_mode.width,
                        b->current_mode.height, b->current_mode.refresh_rate);
            CHECK_EQ(b->current_mode.width, orig.width);
            CHECK_EQ(b->current_mode.height, orig.height);
            CHECK(std::abs(b->current_mode.refresh_rate - orig.refresh_rate) < 1.0);
        }
    }

    return bstest::finish(name);
}

} // namespace bdtest
