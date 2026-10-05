#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "brodisplays/types.h"

namespace brodisplays {

class MacGamma {
public:
    MacGamma();
    ~MacGamma();

    NightLightStatus get_status(uint32_t display_id) const;
    Result set_night_light(uint32_t display_id, bool enabled, uint32_t temperature_kelvin = 4500);
    void restore_all();

private:
    struct SavedTable {
        uint32_t display_id = 0;
        std::vector<float> red;
        std::vector<float> green;
        std::vector<float> blue;
        uint32_t table_size = 0;
        bool has_original = false;
        bool night_light_enabled = false;
        uint32_t temperature_kelvin = 6500;
    };

    SavedTable* find_or_create(uint32_t display_id);

    std::vector<SavedTable> saved_tables_;
};

} // namespace brodisplays
