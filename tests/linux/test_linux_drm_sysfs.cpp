#include "check.h"
#include "linux/drm_sysfs_backend.h"

#include <filesystem>
#include <fstream>

int main() {
    std::printf("[test_linux_drm_sysfs] Starting DRM sysfs verification...\n");

    if (!brodisplays::DrmSysfsBackend::is_available()) {
        bstest::skip("test_linux_drm_sysfs", "/sys/class/drm is not available on this machine");
    }

    auto displays = brodisplays::DrmSysfsBackend::enumerate();
    std::printf("Found %zu connected display(s) via /sys/class/drm\n", displays.size());

    // Count connected in /sys/class/drm
    size_t expected_connected = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm")) {
        auto status_file = entry.path() / "status";
        if (std::filesystem::exists(status_file)) {
            std::ifstream f(status_file);
            std::string st;
            f >> st;
            if (st == "connected") {
                ++expected_connected;
            }
        }
    }

    std::printf("Expected connected outputs according to /sys/class/drm: %zu\n", expected_connected);
    CHECK_EQ(displays.size(), expected_connected);

    for (const auto& d : displays) {
        std::printf("  Connector: %s, Name: %s, Current mode: %ux%u @ %.1f Hz\n",
            d.id.c_str(), d.name.c_str(),
            d.current_mode.width, d.current_mode.height, d.current_mode.refresh_rate);
        CHECK(!d.id.empty());
        CHECK(d.current_mode.width > 0);
        CHECK(d.current_mode.height > 0);
        CHECK(!d.available_modes.empty());
    }

    return bstest::finish("test_linux_drm_sysfs");
}
