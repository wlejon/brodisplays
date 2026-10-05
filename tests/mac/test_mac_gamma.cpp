#include "check.h"
#include "brodisplays/display_service.h"

int main() {
    const char* name = "test_mac_gamma";
    std::printf("[%s] Starting macOS gamma / night shift test...\n", name);
    bstest::require_mutation_opt_in(name, "the night-shift test tints the main display's transfer table and");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    const auto* primary = snap.primary_display();
    if (!primary) bstest::skip(name, "no display attached to tint");
    std::printf("Primary display night light supported: %d, enabled: %d, temp: %u K\n",
        primary->night_light.supported, primary->night_light.enabled, primary->night_light.temperature_kelvin);

    // A display that takes no transfer table (a virtual display without a
    // gamma LUT) cannot be tested here: that is a skip, not a pass.
    auto res_warm = service->set_night_light(true, 4500);
    if (!res_warm.ok) bstest::skip(name, "the display takes no transfer table: " + res_warm.error);
    std::printf("Successfully set 4500K night shift table\n");

    auto warm = service->snapshot();
    if (const auto* p = warm.primary_display()) {
        CHECK(p->night_light.enabled);
        CHECK_EQ(p->night_light.temperature_kelvin, 4500u);
    }

    auto res_restore = service->set_night_light(false, 6500);
    CHECK(res_restore.ok);

    return bstest::finish(name);
}
