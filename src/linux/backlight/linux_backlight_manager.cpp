#include "linux_backlight_manager.h"

#include <algorithm>

#include "linux_backlight_watcher.h"
#include "sysfs_backlight.h"

namespace brodisplays {

std::unique_ptr<BacklightManager> BacklightManager::create() {
    return std::make_unique<LinuxBacklightManager>();
}

LinuxBacklightManager::LinuxBacklightManager() = default;
LinuxBacklightManager::~LinuxBacklightManager() = default;

std::vector<BacklightDevice> LinuxBacklightManager::enumerate_devices() const {
    return enumerate_sysfs_backlight_devices();
}

std::optional<BacklightDevice> LinuxBacklightManager::find_device_by_name(std::string_view name) const {
    return read_sysfs_backlight_device(name);
}

std::optional<BacklightDevice> LinuxBacklightManager::find_device_for_connector(std::string_view connector_name) const {
    auto devices = enumerate_sysfs_backlight_devices();
    std::vector<BacklightDevice> matches;
    for (auto& dev : devices) {
        if (dev.associated_connector == connector_name) {
            matches.push_back(std::move(dev));
        }
    }

    if (matches.empty()) {
        return std::nullopt;
    }

    // Sort by priority: Raw > Platform > Firmware
    std::sort(matches.begin(), matches.end(), [](const BacklightDevice& a, const BacklightDevice& b) {
        return backlight_type_priority(a.type) > backlight_type_priority(b.type);
    });

    return matches.front();
}

std::optional<BacklightDevice> LinuxBacklightManager::primary_device() const {
    auto devices = enumerate_sysfs_backlight_devices();
    if (devices.empty()) {
        return std::nullopt;
    }

    // 1. First priority: device associated with internal panel (eDP/LVDS/DSI)
    std::vector<BacklightDevice> internal_matches;
    for (auto& dev : devices) {
        if (!dev.associated_connector.empty() &&
            (dev.associated_connector.find("eDP") != std::string::npos ||
             dev.associated_connector.find("LVDS") != std::string::npos ||
             dev.associated_connector.find("DSI") != std::string::npos)) {
            internal_matches.push_back(std::move(dev));
        }
    }

    if (!internal_matches.empty()) {
        std::sort(internal_matches.begin(), internal_matches.end(), [](const BacklightDevice& a, const BacklightDevice& b) {
            return backlight_type_priority(a.type) > backlight_type_priority(b.type);
        });
        return internal_matches.front();
    }

    // 2. Fallback: highest priority type among all discovered devices
    std::sort(devices.begin(), devices.end(), [](const BacklightDevice& a, const BacklightDevice& b) {
        return backlight_type_priority(a.type) > backlight_type_priority(b.type);
    });

    return devices.front();
}

std::optional<BacklightDevice> LinuxBacklightManager::resolve_device(std::string_view device_or_connector) const {
    if (device_or_connector.empty()) {
        return primary_device();
    }

    auto dev = find_device_by_name(device_or_connector);
    if (dev) return dev;

    return find_device_for_connector(device_or_connector);
}

Result LinuxBacklightManager::get_brightness(std::string_view device_or_connector,
                                            uint32_t& out_raw,
                                            double& out_normalized,
                                            BrightnessCurve curve) const {
    auto dev = resolve_device(device_or_connector);
    if (!dev) {
        return Result::failure("No backlight device found for: " + std::string(device_or_connector));
    }

    // Re-read directly from sysfs to ensure live hardware reading
    auto fresh = read_sysfs_backlight_device(dev->name);
    if (fresh) {
        *dev = std::move(*fresh);
    }

    out_raw = dev->current_brightness;
    out_normalized = dev->to_normalized(out_raw, curve);
    return Result::success();
}

Result LinuxBacklightManager::set_brightness(std::string_view device_or_connector, uint32_t raw_brightness) {
    auto dev = resolve_device(device_or_connector);
    if (!dev) {
        return Result::failure("No backlight device found for: " + std::string(device_or_connector));
    }

    const uint32_t clamped = std::clamp(raw_brightness, dev->min_brightness, dev->max_brightness);

    // 1. Try systemd-logind via D-Bus
    Result logind_res = logind_client_.set_brightness(dev->name, clamped);
    if (logind_res.ok) {
        return Result::success();
    }

    // 2. Direct sysfs write fallback when permissions allow
    Result sysfs_res = direct_sysfs_write_brightness(dev->name, clamped);
    if (sysfs_res.ok) {
        return Result::success();
    }

    return Result::failure("Failed to set backlight for " + dev->name +
                           ": logind: " + logind_res.error +
                           "; direct sysfs: " + sysfs_res.error);
}

Result LinuxBacklightManager::set_normalized_brightness(std::string_view device_or_connector,
                                                       double normalized,
                                                       BrightnessCurve curve) {
    auto dev = resolve_device(device_or_connector);
    if (!dev) {
        return Result::failure("No backlight device found for: " + std::string(device_or_connector));
    }

    const uint32_t raw = dev->to_raw(normalized, curve);
    return set_brightness(dev->name, raw);
}

std::unique_ptr<BacklightWatcher> LinuxBacklightManager::create_watcher(
    std::function<void(const BrightnessEvent&)> on_event) {
    return create_watcher("", std::move(on_event));
}

std::unique_ptr<BacklightWatcher> LinuxBacklightManager::create_watcher(
    std::string_view device_or_connector,
    std::function<void(const BrightnessEvent&)> on_event) {
    return std::make_unique<LinuxBacklightWatcher>(std::string(device_or_connector), std::move(on_event));
}

std::unique_ptr<BacklightWatcher> LinuxBacklightManager::create_watcher(
    MessageQueue<BrightnessEvent>& queue) {
    return create_watcher("", queue);
}

std::unique_ptr<BacklightWatcher> LinuxBacklightManager::create_watcher(
    std::string_view device_or_connector,
    MessageQueue<BrightnessEvent>& queue) {
    return create_watcher(device_or_connector, [&queue](const BrightnessEvent& ev) {
        queue.push(ev);
    });
}

std::unique_ptr<BacklightWatcher> LinuxBacklightManager::create_watcher(
    MessageQueue<DisplayEvent>& queue) {
    return create_watcher("", queue);
}

std::unique_ptr<BacklightWatcher> LinuxBacklightManager::create_watcher(
    std::string_view device_or_connector,
    MessageQueue<DisplayEvent>& queue) {
    return create_watcher(device_or_connector, [&queue](const BrightnessEvent& ev) {
        queue.push(ev);
    });
}

} // namespace brodisplays
