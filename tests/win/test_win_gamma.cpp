#include "check.h"
#include "brodisplays/display_service.h"

int main() {
    std::printf("[test_win_gamma] Starting Windows gamma / night light test...\n");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    REQUIRE(!snap.displays.empty());

    // Check night light status
    const auto* primary = snap.primary_display();
    REQUIRE(primary != nullptr);
    std::printf("Primary display night light supported: %d, enabled: %d, temp: %u K\n",
        primary->night_light.supported, primary->night_light.enabled, primary->night_light.temperature_kelvin);

    // Apply warm night light (4500K)
    auto res_warm = service->set_night_light(true, 4500);
    if (!res_warm.ok) {
        std::printf("Note: Driver reported gamma ramp modification unavailable: %s\n", res_warm.error.c_str());
    } else {
        std::printf("Successfully set 4500K night light ramp\n");
    }

    // Restore standard 6500K daylight
    auto res_restore = service->set_night_light(false, 6500);
    CHECK(res_restore.ok);

    return bstest::finish("test_win_gamma");
}
