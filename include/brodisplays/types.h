#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace brodisplays {

struct Result {
    bool ok = true;
    std::string error;

    static Result success() {
        return Result{true, {}};
    }

    static Result failure(std::string msg) {
        return Result{false, std::move(msg)};
    }

    explicit operator bool() const noexcept {
        return ok;
    }
};

enum class DisplayOrientation : uint32_t {
    Normal = 0,
    Rotate90 = 90,
    Rotate180 = 180,
    Rotate270 = 270,
};

struct DisplayGeometry {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    bool operator==(const DisplayGeometry& other) const noexcept = default;
};

struct DisplayMode {
    uint32_t width = 0;
    uint32_t height = 0;
    double refresh_rate = 0.0; // In Hz, e.g. 60.0, 144.0

    bool operator==(const DisplayMode& other) const noexcept = default;
};

struct DisplayScale {
    double factor = 1.0; // 1.0, 1.25, 1.5, 2.0
    int32_t dpi = 96;     // 96, 120, 144, 192

    bool operator==(const DisplayScale& other) const noexcept = default;
};

struct HdrCapabilities {
    bool supported = false;
    bool enabled = false;
    float max_luminance_nits = 0.0f;
    float min_luminance_nits = 0.0f;

    bool operator==(const HdrCapabilities& other) const noexcept = default;
};

struct NightLightStatus {
    bool supported = false;
    bool enabled = false;
    uint32_t temperature_kelvin = 6500; // e.g. 1200K to 6500K

    bool operator==(const NightLightStatus& other) const noexcept = default;
};

struct ColorProfile {
    std::string name;
    std::string icc_path;

    bool operator==(const ColorProfile& other) const noexcept = default;
};

struct EdidInfo {
    std::string manufacturer_id; // 3-letter PNP code, e.g. "SAM", "DEL"
    uint16_t product_code = 0;
    uint32_t serial_number = 0;
    std::string monitor_name;
    uint32_t width_cm = 0;
    uint32_t height_cm = 0;
    std::vector<uint8_t> raw_bytes;

    bool operator==(const EdidInfo& other) const noexcept = default;
};

struct DisplayInfo {
    std::string id;              // Unique persistent identifier (path or name)
    std::string name;            // User-friendly display name (e.g. "Dell U2720Q")
    std::string adapter_name;    // GPU / adapter (e.g. "NVIDIA GeForce RTX 4090")
    std::string manufacturer;
    std::string model;
    // The name the OS's window system knows this display by, for joining
    // with other APIs: the GDI source device on Windows ("\\.\DISPLAY1",
    // MONITORINFOEX::szDevice), the CGDirectDisplayID in decimal on macOS,
    // the output / connector name on Linux ("DP-1").
    std::string device_name;
    bool is_primary = false;
    bool is_internal = false;     // e.g. laptop eDP, built-in display
    bool is_connected = true;
    // Showing an image right now: false while the display sleeps (macOS
    // reports sleeping displays; their geometry is then not meaningful).
    bool is_active = true;
    // Non-empty when this display mirrors another one (clone mode): the id of
    // the display whose desktop area it shows. A mirror occupies no desktop
    // area of its own.
    std::string mirror_of;

    DisplayGeometry geometry;
    uint32_t physical_width_mm = 0;
    uint32_t physical_height_mm = 0;

    DisplayOrientation orientation = DisplayOrientation::Normal;
    DisplayScale scale;
    DisplayMode current_mode;
    std::vector<DisplayMode> available_modes;

    HdrCapabilities hdr;
    NightLightStatus night_light;
    ColorProfile color_profile;
    EdidInfo edid;

    bool operator==(const DisplayInfo& other) const noexcept = default;
};

struct DisplaysSnapshot {
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::vector<DisplayInfo> displays;

    // Same displays, same state (the timestamp is not compared).
    bool same_displays(const DisplaysSnapshot& other) const noexcept {
        return displays == other.displays;
    }

    const DisplayInfo* find_display(const std::string& display_id) const noexcept {
        for (const auto& d : displays) {
            if (d.id == display_id) return &d;
        }
        return nullptr;
    }

    const DisplayInfo* primary_display() const noexcept {
        for (const auto& d : displays) {
            if (d.is_primary) return &d;
        }
        if (!displays.empty()) return &displays.front();
        return nullptr;
    }
};

struct DisplayConfigChange {
    std::string display_id;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
    std::optional<double> refresh_rate;
    std::optional<int32_t> x;
    std::optional<int32_t> y;
    std::optional<DisplayOrientation> orientation;
    std::optional<double> scale_factor;
};

} // namespace brodisplays
