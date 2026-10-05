#import "mac_gamma.h"

#import <CoreGraphics/CoreGraphics.h>
#import <algorithm>
#import <cmath>

#include "common/color_temperature.h"

namespace brodisplays {

MacGamma::MacGamma() = default;

MacGamma::~MacGamma() {
    restore_all();
}

MacGamma::SavedTable* MacGamma::find_or_create(uint32_t display_id) {
    for (auto& st : saved_tables_) {
        if (st.display_id == display_id) return &st;
    }
    saved_tables_.push_back(SavedTable{display_id, {}, {}, {}, 0, false, false, 6500});
    return &saved_tables_.back();
}

NightLightStatus MacGamma::get_status(uint32_t display_id) const {
    NightLightStatus st;
    st.supported = true;
    for (const auto& table : saved_tables_) {
        if (table.display_id == display_id) {
            st.enabled = table.night_light_enabled;
            st.temperature_kelvin = table.temperature_kelvin;
            return st;
        }
    }
    st.enabled = false;
    st.temperature_kelvin = 6500;
    return st;
}

Result MacGamma::set_night_light(uint32_t display_id, bool enabled, uint32_t temperature_kelvin) {
    CGDirectDisplayID d = static_cast<CGDirectDisplayID>(display_id);
    SavedTable* st = find_or_create(display_id);

    constexpr uint32_t kCapacity = 256;
    if (!st->has_original) {
        st->red.resize(kCapacity);
        st->green.resize(kCapacity);
        st->blue.resize(kCapacity);
        uint32_t sample_count = 0;
        CGError err = CGGetDisplayTransferByTable(
            d, kCapacity, st->red.data(), st->green.data(), st->blue.data(), &sample_count);
        if (err == kCGErrorSuccess && sample_count > 0) {
            st->table_size = sample_count;
            st->red.resize(sample_count);
            st->green.resize(sample_count);
            st->blue.resize(sample_count);
            st->has_original = true;
        } else {
            // Default linear table
            st->table_size = kCapacity;
            for (uint32_t i = 0; i < kCapacity; ++i) {
                float v = static_cast<float>(i) / (kCapacity - 1);
                st->red[i] = v;
                st->green[i] = v;
                st->blue[i] = v;
            }
            st->has_original = true;
        }
    }

    if (!enabled) {
        CGSetDisplayTransferByTable(
            d, st->table_size, st->red.data(), st->green.data(), st->blue.data());
        st->night_light_enabled = false;
        st->temperature_kelvin = 6500;
        return Result::success();
    }

    double r_factor = 1.0, g_factor = 1.0, b_factor = 1.0;
    kelvin_to_rgb(temperature_kelvin, r_factor, g_factor, b_factor);

    std::vector<float> new_r = st->red;
    std::vector<float> new_g = st->green;
    std::vector<float> new_b = st->blue;

    for (size_t i = 0; i < st->table_size; ++i) {
        new_r[i] = std::clamp(static_cast<float>(st->red[i] * r_factor), 0.0f, 1.0f);
        new_g[i] = std::clamp(static_cast<float>(st->green[i] * g_factor), 0.0f, 1.0f);
        new_b[i] = std::clamp(static_cast<float>(st->blue[i] * b_factor), 0.0f, 1.0f);
    }

    CGError err = CGSetDisplayTransferByTable(
        d, st->table_size, new_r.data(), new_g.data(), new_b.data());
    if (err != kCGErrorSuccess) {
        return Result::failure("CGSetDisplayTransferByTable failed: " + std::to_string(err));
    }

    st->night_light_enabled = true;
    st->temperature_kelvin = temperature_kelvin;
    return Result::success();
}

void MacGamma::restore_all() {
    for (auto& st : saved_tables_) {
        if (st.has_original && st.night_light_enabled) {
            CGDirectDisplayID d = static_cast<CGDirectDisplayID>(st.display_id);
            CGSetDisplayTransferByTable(
                d, st.table_size, st.red.data(), st.green.data(), st.blue.data());
            st.night_light_enabled = false;
        }
    }
}

} // namespace brodisplays
