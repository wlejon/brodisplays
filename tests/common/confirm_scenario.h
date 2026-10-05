#pragma once

// Shared body of the per-platform confirm-persistence tests: switch the
// primary display to another mode temporarily, check the platform's store of
// persisted configurations still holds the original, confirm, check the store
// now holds the new mode, then put the original back the same way (applied
// and confirmed, so the store holds the original again) and check both.
// Opt-in only (BRODISPLAYS_TEST_MUTATE=1): it visibly changes the display and
// rewrites the persisted configuration (restoring it before it returns).

#include "check.h"
#include "common/mode_candidates.h"
#include "brodisplays/display_service.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace bdtest {

// Reads the mode the platform would restore for `display` at the next
// login / reconnect; nullopt when it cannot be read.
using PersistedModeReader = std::function<std::optional<brodisplays::DisplayMode>(const brodisplays::DisplayInfo&)>;

// A persisted-mode reader returns this rate when the store records no rate
// (it keeps "the display's default"): only the size is compared then.
inline constexpr double kAnyRate = -1.0;

inline bool same_mode(const brodisplays::DisplayMode& a, const brodisplays::DisplayMode& b) {
    const bool rate_ok = a.refresh_rate == kAnyRate || b.refresh_rate == kAnyRate ||
                         std::abs(a.refresh_rate - b.refresh_rate) < 1.0;
    return a.width == b.width && a.height == b.height && rate_ok;
}

inline void print_mode(const char* what, const std::optional<brodisplays::DisplayMode>& m) {
    if (m) std::printf("%s: %ux%u @ %.2f Hz\n", what, m->width, m->height, m->refresh_rate);
    else std::printf("%s: (unreadable)\n", what);
}

// The store may be written asynchronously (macOS writes its preferences
// through cfprefsd); poll it for a while.
inline bool persisted_becomes(const PersistedModeReader& read, const brodisplays::DisplayInfo& d,
                              const brodisplays::DisplayMode& want) {
    return bstest::wait_until([&] {
        auto m = read(d);
        return m && same_mode(*m, want);
    }, std::chrono::milliseconds(5000), std::chrono::milliseconds(100));
}

inline int run_confirm_scenario(const char* name, const PersistedModeReader& read_persisted) {
    bstest::require_mutation_opt_in(name, "confirm-persistence switches the primary display mode, saves it, and");

    auto service = brodisplays::DisplayService::create();
    REQUIRE(service != nullptr);
    auto snap = service->snapshot();
    const auto* primary = snap.primary_display();
    if (!primary) bstest::skip(name, "no display available to change");
    const brodisplays::DisplayInfo original = *primary;
    const brodisplays::DisplayMode orig = original.current_mode;

    auto stored_before = read_persisted(original);
    print_mode("Active mode", orig);
    print_mode("Persisted mode", stored_before);
    if (!stored_before) bstest::skip(name, "the persisted configuration of the primary display cannot be read");

    const auto candidates = alternate_modes(original);
    if (candidates.empty()) bstest::skip(name, "the primary display offers no alternate mode");
    std::string refusals;
    const int picked = apply_first_accepted(*service, original.id, candidates, std::chrono::seconds(30), &refusals);
    if (picked < 0) bstest::skip(name, "the platform refused every alternate mode: " + refusals);
    const brodisplays::DisplayMode target = candidates[static_cast<size_t>(picked)];
    print_mode("Temporary mode", target);

    // Not persisted while it is only a test.
    auto during = read_persisted(original);
    print_mode("Persisted while pending", during);
    CHECK(during && same_mode(*during, *stored_before));

    auto confirmed = service->confirm_configuration();
    if (!confirmed.ok) std::printf("confirm_configuration: %s\n", confirmed.error.c_str());
    CHECK(confirmed.ok);
    CHECK(!service->is_revert_pending());
    bool saved = persisted_becomes(read_persisted, original, target);
    print_mode("Persisted after confirm", read_persisted(original));
    CHECK(saved);

    // Restore: the original mode, persisted again.
    auto restore = service->apply_temporary_configuration(change_to(original.id, orig), std::chrono::seconds(30));
    if (!restore.ok) std::printf("restore apply: %s\n", restore.error.c_str());
    CHECK(restore.ok);
    auto restore_confirm = service->confirm_configuration();
    if (!restore_confirm.ok) std::printf("restore confirm: %s\n", restore_confirm.error.c_str());
    CHECK(restore_confirm.ok);
    bool restored_store = persisted_becomes(read_persisted, original, *stored_before);
    print_mode("Persisted after restore", read_persisted(original));
    CHECK(restored_store);

    auto after = service->snapshot();
    const auto* back = after.find_display(original.id);
    REQUIRE(back != nullptr);
    print_mode("Active after restore", back->current_mode);
    CHECK(same_mode(back->current_mode, orig));

    return bstest::finish(name);
}

} // namespace bdtest
