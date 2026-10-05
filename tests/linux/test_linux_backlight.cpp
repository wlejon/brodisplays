#include "check.h"
#include "brodisplays/backlight.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    std::printf("[test_linux_backlight] Starting Linux backlight integration test...\n");

    auto manager = brodisplays::BacklightManager::create();
    REQUIRE(manager != nullptr);

    auto devices = manager->enumerate_devices();
    std::printf("[test_linux_backlight] Discovered %zu backlight device(s) in /sys/class/backlight\n",
                devices.size());

    // MANDATE: If no physical backlight device exists on this machine, report skip code 77
    if (devices.empty()) {
        bstest::skip("test_linux_backlight",
                     "No physical backlight device found in /sys/class/backlight "
                     "(desktop host with external monitor and no laptop panel)");
    }

    // Physical device verification
    for (const auto& dev : devices) {
        std::printf("  Device: %s [type: %s, connector: %s]\n",
                    dev.name.c_str(),
                    std::string(brodisplays::to_string(dev.type)).c_str(),
                    dev.associated_connector.empty() ? "(none)" : dev.associated_connector.c_str());
        std::printf("    Brightness: %u / %u (min: %u, normalized: %.2f%%)\n",
                    dev.current_brightness, dev.max_brightness, dev.min_brightness,
                    dev.normalized_brightness * 100.0);

        CHECK(!dev.name.empty());
        CHECK(dev.max_brightness >= dev.min_brightness);
        CHECK(dev.current_brightness >= dev.min_brightness);
        CHECK(dev.current_brightness <= dev.max_brightness);
        CHECK(dev.normalized_brightness >= 0.0 && dev.normalized_brightness <= 1.0);

        // Lookup by name
        auto by_name = manager->find_device_by_name(dev.name);
        REQUIRE(by_name.has_value());
        CHECK_EQ(by_name->name, dev.name);
        CHECK_EQ(by_name->max_brightness, dev.max_brightness);

        // Lookup by connector if associated
        if (!dev.associated_connector.empty()) {
            auto by_conn = manager->find_device_for_connector(dev.associated_connector);
            CHECK(by_conn.has_value());
        }

        // Query brightness directly
        uint32_t cur_raw = 0;
        double cur_norm = 0.0;
        brodisplays::Result res = manager->get_brightness(dev.name, cur_raw, cur_norm);
        CHECK(res.ok);
        CHECK(cur_raw >= dev.min_brightness && cur_raw <= dev.max_brightness);
    }

    // Primary device verification
    auto primary = manager->primary_device();
    REQUIRE(primary.has_value());
    std::printf("[test_linux_backlight] Primary backlight device: %s\n", primary->name.c_str());

    // Watcher lifecycle verification
    brodisplays::MessageQueue<brodisplays::BrightnessEvent> b_queue;
    auto watcher = manager->create_watcher(primary->name, b_queue);
    REQUIRE(watcher != nullptr);
    CHECK(watcher->is_running());

    // Optional mutation test (only if user opted-in via BRODISPLAYS_TEST_MUTATE=1)
    if (bstest::mutation_opted_in()) {
        std::printf("[test_linux_backlight] Testing live brightness modification...\n");
        uint32_t original_raw = primary->current_brightness;

        // Apply same brightness to exercise logind / sysfs path safely
        brodisplays::Result set_res = manager->set_brightness(primary->name, original_raw);
        CHECK(set_res.ok);
        if (!set_res.ok) {
            std::printf("  Failed to set brightness: %s\n", set_res.error.c_str());
        }
    }

    watcher->stop();
    CHECK(!watcher->is_running());

    return bstest::finish("test_linux_backlight");
}
