#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "brodisplays/backlight.h"
#include "brodisplays/types.h"

namespace brodisplays {

// Checks whether /sys/class/backlight exists
bool is_sysfs_backlight_available();

// Enumerates all device names in /sys/class/backlight
std::vector<std::string> enumerate_sysfs_backlight_names();

// Reads properties of a single backlight device by name
std::optional<BacklightDevice> read_sysfs_backlight_device(std::string_view name);

// Enumerates all backlight devices with full metadata
std::vector<BacklightDevice> enumerate_sysfs_backlight_devices();

// Attempts direct write to /sys/class/backlight/<name>/brightness
Result direct_sysfs_write_brightness(std::string_view name, uint32_t raw_brightness);

// Detects the DRM connector (e.g. "eDP-1") associated with this backlight device
std::string find_associated_connector(std::string_view backlight_name,
                                      const std::filesystem::path& backlight_dir);

// Priority ranking: Raw (3) > Platform (2) > Firmware (1) > Unknown (0)
int backlight_type_priority(BacklightType type) noexcept;

} // namespace brodisplays
