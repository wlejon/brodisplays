#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "brodisplays/types.h"

namespace brodisplays {

class WinGamma {
public:
    WinGamma();
    ~WinGamma();

    NightLightStatus get_status(const std::string& gdi_device_name) const;

    Result set_night_light(
        const std::string& gdi_device_name,
        bool enabled,
        uint32_t temperature_kelvin = 4500);

    void restore_all();

private:
    struct SavedRamp {
        std::string device_name;
        uint16_t ramp[3][256];
        bool has_original = false;
        bool night_light_enabled = false;
        uint32_t temperature_kelvin = 6500;
    };

    SavedRamp* find_or_create(const std::string& device_name);

    std::vector<SavedRamp> saved_ramps_;
};

} // namespace brodisplays
