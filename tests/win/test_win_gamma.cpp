#include "check.h"
#include "brodisplays/display_service.h"

int main() {
    const char* name = "test_win_gamma";
    std::printf("[%s] Starting Windows gamma / night light test...\n", name);
    bstest::require_mutation_opt_in(name, "the night-light test tints the primary display's gamma ramp and");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    const auto* primary = snap.primary_display();
    if (!primary) bstest::skip(name, "no display attached to tint");
    std::printf("Primary display night light supported: %d, enabled: %d, temp: %u K\n",
        primary->night_light.supported, primary->night_light.enabled, primary->night_light.temperature_kelvin);

    // A display whose driver takes no gamma ramp (a basic display adapter, a
    // remote session) cannot be tested here: that is a skip, not a pass.
    auto res_warm = service->set_night_light(true, 4500);
    if (!res_warm.ok) bstest::skip(name, "the driver takes no gamma ramp: " + res_warm.error);
    std::printf("Successfully set 4500K night light ramp\n");

    auto warm = service->snapshot();
    if (const auto* p = warm.primary_display()) {
        CHECK(p->night_light.enabled);
        CHECK_EQ(p->night_light.temperature_kelvin, 4500u);
    }

    // Restore standard 6500K daylight
    auto res_restore = service->set_night_light(false, 6500);
    CHECK(res_restore.ok);

    return bstest::finish(name);
}
