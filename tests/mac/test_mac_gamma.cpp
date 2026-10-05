#include "check.h"
#include "brodisplays/display_service.h"

int main() {
    std::printf("[test_mac_gamma] Starting macOS gamma / night shift test...\n");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    REQUIRE(!snap.displays.empty());

    const auto* primary = snap.primary_display();
    REQUIRE(primary != nullptr);
    std::printf("Primary display night light supported: %d, enabled: %d, temp: %u K\n",
        primary->night_light.supported, primary->night_light.enabled, primary->night_light.temperature_kelvin);

    auto res_warm = service->set_night_light(true, 4500);
    if (!res_warm.ok) {
        std::printf("Note: set_night_light returned: %s\n", res_warm.error.c_str());
    } else {
        std::printf("Successfully set 4500K night shift table\n");
    }

    auto res_restore = service->set_night_light(false, 6500);
    CHECK(res_restore.ok);

    return bstest::finish("test_mac_gamma");
}
