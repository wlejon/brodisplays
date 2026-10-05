#include "sysfs_backlight.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace brodisplays {

namespace fs = std::filesystem;

namespace {

std::string trim_str(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    return s.substr(start);
}

std::optional<std::string> read_file_string(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return std::nullopt;
    std::string line;
    if (std::getline(f, line)) {
        return trim_str(std::move(line));
    }
    return std::nullopt;
}

std::optional<uint32_t> read_file_uint(const fs::path& p) {
    auto str = read_file_string(p);
    if (!str || str->empty()) return std::nullopt;
    try {
        unsigned long val = std::stoul(*str);
        return static_cast<uint32_t>(val);
    } catch (...) {
        return std::nullopt;
    }
}

bool is_internal_connector_name(std::string_view name) {
    return name.find("eDP") != std::string_view::npos ||
           name.find("LVDS") != std::string_view::npos ||
           name.find("DSI") != std::string_view::npos;
}

std::string extract_connector_from_drm_dirname(std::string_view dirname) {
    // dirname e.g. "card0-eDP-1" -> "eDP-1"
    auto dash = dirname.find('-');
    if (dash != std::string_view::npos) {
        return std::string(dirname.substr(dash + 1));
    }
    return std::string(dirname);
}

} // namespace

bool is_sysfs_backlight_available() {
    std::error_code ec;
    return fs::is_directory("/sys/class/backlight", ec);
}

std::vector<std::string> enumerate_sysfs_backlight_names() {
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::is_directory("/sys/class/backlight", ec)) {
        return names;
    }

    for (const auto& entry : fs::directory_iterator("/sys/class/backlight", ec)) {
        if (entry.is_directory(ec) || entry.is_symlink(ec)) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string find_associated_connector(std::string_view /*backlight_name*/,
                                      const fs::path& backlight_dir) {
    std::error_code ec;

    // Strategy 1: Check canonical path hierarchy for cardX-<connector>
    fs::path canonical_bl = fs::canonical(backlight_dir, ec);
    if (!ec) {
        for (const auto& elem : canonical_bl) {
            std::string elem_str = elem.string();
            if (elem_str.rfind("card", 0) == 0 && elem_str.find('-') != std::string::npos) {
                return extract_connector_from_drm_dirname(elem_str);
            }
        }
    }

    // Strategy 2: Match parent GPU device with DRM connectors
    fs::path device_symlink = backlight_dir / "device";
    fs::path parent_device = fs::canonical(device_symlink, ec);

    std::string matched_internal;
    std::string matched_fallback;

    if (!ec && fs::is_directory("/sys/class/drm", ec)) {
        for (const auto& drm_entry : fs::directory_iterator("/sys/class/drm", ec)) {
            std::string drm_name = drm_entry.path().filename().string();
            if (drm_name.rfind("card", 0) != 0 || drm_name.find('-') == std::string::npos) {
                continue;
            }

            fs::path drm_canonical = fs::canonical(drm_entry.path(), ec);
            if (ec) continue;

            // Check if this connector is under parent_device or shares parent_device
            bool is_match = false;
            if (drm_canonical.string().find(parent_device.string()) != std::string::npos) {
                is_match = true;
            } else {
                fs::path connector_device = fs::canonical(drm_entry.path() / "device", ec);
                if (!ec && connector_device == parent_device) {
                    is_match = true;
                }
            }

            if (is_match) {
                std::string conn = extract_connector_from_drm_dirname(drm_name);
                if (is_internal_connector_name(conn)) {
                    matched_internal = conn;
                    break;
                }
                if (matched_fallback.empty()) {
                    matched_fallback = conn;
                }
            }
        }
    }

    if (!matched_internal.empty()) return matched_internal;
    if (!matched_fallback.empty()) return matched_fallback;

    // Strategy 3: For platform/firmware backlights or fallback, find any internal DRM connector
    if (fs::is_directory("/sys/class/drm", ec)) {
        for (const auto& drm_entry : fs::directory_iterator("/sys/class/drm", ec)) {
            std::string drm_name = drm_entry.path().filename().string();
            if (drm_name.rfind("card", 0) == 0 && drm_name.find('-') != std::string::npos) {
                std::string conn = extract_connector_from_drm_dirname(drm_name);
                if (is_internal_connector_name(conn)) {
                    return conn;
                }
            }
        }
    }

    return "";
}

int backlight_type_priority(BacklightType type) noexcept {
    switch (type) {
        case BacklightType::Raw:
            return 3;
        case BacklightType::Platform:
            return 2;
        case BacklightType::Firmware:
            return 1;
        case BacklightType::Unknown:
        default:
            return 0;
    }
}

std::optional<BacklightDevice> read_sysfs_backlight_device(std::string_view name) {
    fs::path dev_dir = fs::path("/sys/class/backlight") / std::string(name);
    std::error_code ec;
    if (!fs::is_directory(dev_dir, ec) && !fs::is_symlink(dev_dir, ec)) {
        return std::nullopt;
    }

    auto max_b = read_file_uint(dev_dir / "max_brightness");
    if (!max_b) {
        return std::nullopt;
    }

    // Try actual_brightness first, fall back to brightness
    auto cur_b = read_file_uint(dev_dir / "actual_brightness");
    if (!cur_b) {
        cur_b = read_file_uint(dev_dir / "brightness");
    }
    if (!cur_b) {
        cur_b = 0;
    }

    // min_brightness is optional in sysfs (Linux 6.x+)
    auto min_b = read_file_uint(dev_dir / "min_brightness");
    if (!min_b) {
        min_b = 0;
    }

    auto type_str = read_file_string(dev_dir / "type");
    BacklightType type = type_str ? backlight_type_from_string(*type_str) : BacklightType::Unknown;

    BacklightDevice dev;
    dev.name = std::string(name);
    dev.sysfs_path = dev_dir.string();
    dev.type = type;
    dev.max_brightness = *max_b;
    dev.min_brightness = *min_b;
    dev.current_brightness = std::clamp(*cur_b, dev.min_brightness, dev.max_brightness);
    dev.associated_connector = find_associated_connector(name, dev_dir);
    dev.normalized_brightness = dev.to_normalized(dev.current_brightness, BrightnessCurve::Perceptual);

    return dev;
}

std::vector<BacklightDevice> enumerate_sysfs_backlight_devices() {
    std::vector<BacklightDevice> devices;
    auto names = enumerate_sysfs_backlight_names();
    for (const auto& name : names) {
        auto dev = read_sysfs_backlight_device(name);
        if (dev) {
            devices.push_back(std::move(*dev));
        }
    }
    return devices;
}

Result direct_sysfs_write_brightness(std::string_view name, uint32_t raw_brightness) {
    fs::path p = fs::path("/sys/class/backlight") / std::string(name) / "brightness";
    std::ofstream f(p);
    if (!f.is_open()) {
        return Result::failure("Cannot open " + p.string() + " for writing: " + std::strerror(errno));
    }

    f << raw_brightness << "\n";
    f.flush();
    if (f.fail()) {
        return Result::failure("Failed to write brightness to " + p.string() + ": " + std::strerror(errno));
    }

    return Result::success();
}

} // namespace brodisplays
