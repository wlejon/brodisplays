#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "brodisplays/event_queue.h"
#include "brodisplays/types.h"

namespace brodisplays {

enum class BacklightType {
    Raw,
    Platform,
    Firmware,
    Unknown,
};

std::string_view to_string(BacklightType type) noexcept;
BacklightType backlight_type_from_string(std::string_view str) noexcept;

enum class BrightnessCurve {
    Linear,
    Perceptual,
    Logarithmic = Perceptual,
};

struct BacklightDevice {
    std::string name;                     // e.g. "intel_backlight"
    std::string sysfs_path;               // e.g. "/sys/class/backlight/intel_backlight"
    BacklightType type = BacklightType::Unknown;
    uint32_t current_brightness = 0;      // Current raw brightness
    uint32_t max_brightness = 0;          // Max hardware brightness level
    uint32_t min_brightness = 0;          // Min hardware brightness level (sysfs min_brightness or 0)
    double normalized_brightness = 0.0;   // In [0.0, 1.0], using perceptual curve by default
    std::string associated_connector;    // e.g. "eDP-1"

    bool operator==(const BacklightDevice& other) const noexcept = default;

    // Convert raw brightness to [0.0, 1.0] for this device's range
    double to_normalized(uint32_t raw, BrightnessCurve curve = BrightnessCurve::Perceptual) const noexcept;

    // Convert [0.0, 1.0] to raw brightness value for this device's range
    uint32_t to_raw(double normalized, BrightnessCurve curve = BrightnessCurve::Perceptual) const noexcept;

    // Static curve calculation helpers
    static double calculate_normalized(uint32_t raw, uint32_t min_b, uint32_t max_b,
                                       BrightnessCurve curve = BrightnessCurve::Perceptual) noexcept;

    static uint32_t calculate_raw(double normalized, uint32_t min_b, uint32_t max_b,
                                  BrightnessCurve curve = BrightnessCurve::Perceptual) noexcept;
};

class BacklightWatcher {
public:
    virtual ~BacklightWatcher() = default;

    // Returns true if the background watcher thread is active
    virtual bool is_running() const noexcept = 0;

    // Stops the watcher thread
    virtual void stop() = 0;
};

// Panel backlights (laptop and tablet panels), Linux only: devices come from
// /sys/class/backlight, brightness is set through logind's
// SetBrightness (falling back to writing sysfs directly) and watched with
// inotify. A machine without a panel enumerates no devices.
class BacklightManager {
public:
    // nullptr on Windows and macOS, where brodisplays has no backlight backend.
    static std::unique_ptr<BacklightManager> create();

    virtual ~BacklightManager() = default;

    // Enumerate all discovered backlight devices
    virtual std::vector<BacklightDevice> enumerate_devices() const = 0;

    // Find device by sysfs name (e.g. "intel_backlight")
    virtual std::optional<BacklightDevice> find_device_by_name(std::string_view name) const = 0;

    // Find device associated with a DRM connector (e.g. "eDP-1")
    virtual std::optional<BacklightDevice> find_device_for_connector(std::string_view connector_name) const = 0;

    // The preferred primary backlight device (e.g. internal laptop screen eDP)
    virtual std::optional<BacklightDevice> primary_device() const = 0;

    // Read current raw and normalized brightness for a device or connector name
    // (empty string refers to the primary device)
    virtual Result get_brightness(std::string_view device_or_connector,
                                  uint32_t& out_raw,
                                  double& out_normalized,
                                  BrightnessCurve curve = BrightnessCurve::Perceptual) const = 0;

    // Set raw brightness value (tries logind D-Bus first, then falls back to direct sysfs write)
    // (empty string refers to the primary device)
    virtual Result set_brightness(std::string_view device_or_connector, uint32_t raw_brightness) = 0;

    // Set normalized brightness [0.0, 1.0]
    // (empty string refers to the primary device)
    virtual Result set_normalized_brightness(std::string_view device_or_connector,
                                             double normalized,
                                             BrightnessCurve curve = BrightnessCurve::Perceptual) = 0;

    // Create a watcher that invokes a callback on brightness change
    virtual std::unique_ptr<BacklightWatcher> create_watcher(
        std::function<void(const BrightnessEvent&)> on_event) = 0;

    virtual std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        std::function<void(const BrightnessEvent&)> on_event) = 0;

    // Create a watcher that pushes to a typed BrightnessEvent queue
    virtual std::unique_ptr<BacklightWatcher> create_watcher(
        MessageQueue<BrightnessEvent>& queue) = 0;

    virtual std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        MessageQueue<BrightnessEvent>& queue) = 0;

    // Create a watcher that pushes to the unified DisplayEvent queue
    virtual std::unique_ptr<BacklightWatcher> create_watcher(
        MessageQueue<DisplayEvent>& queue) = 0;

    virtual std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        MessageQueue<DisplayEvent>& queue) = 0;
};

} // namespace brodisplays
