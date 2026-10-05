#include "win_gamma.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace brodisplays {

namespace {

void kelvin_to_rgb(uint32_t kelvin, double& r, double& g, double& b) {
    // Clamp between 1000K and 10000K
    double temp = static_cast<double>(std::clamp(kelvin, 1000u, 10000u)) / 100.0;

    // Red
    if (temp <= 66.0) {
        r = 1.0;
    } else {
        r = temp - 60.0;
        r = 329.698727446 * std::pow(r, -0.1332047592);
        r = std::clamp(r / 255.0, 0.0, 1.0);
    }

    // Green
    if (temp <= 66.0) {
        g = temp;
        g = 99.4708025861 * std::log(g) - 161.1195681661;
        g = std::clamp(g / 255.0, 0.0, 1.0);
    } else {
        g = temp - 60.0;
        g = 288.1221695283 * std::pow(g, -0.0755148492);
        g = std::clamp(g / 255.0, 0.0, 1.0);
    }

    // Blue
    if (temp >= 66.0) {
        b = 1.0;
    } else if (temp <= 19.0) {
        b = 0.0;
    } else {
        b = temp - 10.0;
        b = 138.5177312231 * std::log(b) - 305.0447927307;
        b = std::clamp(b / 255.0, 0.0, 1.0);
    }
}

std::wstring to_wide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring w(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &w[0], size);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

// A DC for one display's gamma ramp: CreateDC on the GDI device name, or the
// screen DC as a fallback. The two are released differently.
struct DisplayDC {
    HDC hdc = nullptr;
    bool from_get_dc = false;

    explicit DisplayDC(const std::string& device_name) {
        std::wstring wname = to_wide(device_name);
        hdc = CreateDCW(L"DISPLAY", wname.empty() ? nullptr : wname.c_str(), nullptr, nullptr);
        if (!hdc) {
            hdc = GetDC(nullptr);
            from_get_dc = hdc != nullptr;
        }
    }
    ~DisplayDC() {
        if (!hdc) return;
        if (from_get_dc) {
            ReleaseDC(nullptr, hdc);
        } else {
            DeleteDC(hdc);
        }
    }
    DisplayDC(const DisplayDC&) = delete;
    DisplayDC& operator=(const DisplayDC&) = delete;
};

} // namespace

WinGamma::WinGamma() = default;

WinGamma::~WinGamma() {
    restore_all();
}

WinGamma::SavedRamp* WinGamma::find_or_create(const std::string& device_name) {
    for (auto& sr : saved_ramps_) {
        if (sr.device_name == device_name) return &sr;
    }
    saved_ramps_.push_back(SavedRamp{device_name, {}, false, false, 6500});
    return &saved_ramps_.back();
}

NightLightStatus WinGamma::get_status(const std::string& device_name) const {
    NightLightStatus st;
    st.supported = true;
    for (const auto& sr : saved_ramps_) {
        if (sr.device_name == device_name) {
            st.enabled = sr.night_light_enabled;
            st.temperature_kelvin = sr.temperature_kelvin;
            return st;
        }
    }
    st.enabled = false;
    st.temperature_kelvin = 6500;
    return st;
}

Result WinGamma::set_night_light(
    const std::string& device_name,
    bool enabled,
    uint32_t temperature_kelvin) {
    SavedRamp* sr = find_or_create(device_name);

    DisplayDC dc(device_name);
    HDC hdc = dc.hdc;
    if (!hdc) {
        return Result::failure("Failed to acquire device context for display gamma");
    }

    if (!sr->has_original) {
        if (GetDeviceGammaRamp(hdc, sr->ramp)) {
            sr->has_original = true;
        } else {
            // Default linear ramp if GetDeviceGammaRamp fails
            for (int i = 0; i < 256; ++i) {
                uint16_t val = static_cast<uint16_t>((i * 65535) / 255);
                sr->ramp[0][i] = val;
                sr->ramp[1][i] = val;
                sr->ramp[2][i] = val;
            }
            sr->has_original = true;
        }
    }

    if (!enabled) {
        // Restore original ramp
        if (!SetDeviceGammaRamp(hdc, sr->ramp)) {
            return Result::failure("SetDeviceGammaRamp failed to restore the original ramp");
        }
        sr->night_light_enabled = false;
        sr->temperature_kelvin = 6500;
        return Result::success();
    }

    double r_factor = 1.0, g_factor = 1.0, b_factor = 1.0;
    kelvin_to_rgb(temperature_kelvin, r_factor, g_factor, b_factor);

    uint16_t new_ramp[3][256];
    for (int i = 0; i < 256; ++i) {
        new_ramp[0][i] = static_cast<uint16_t>(std::clamp(sr->ramp[0][i] * r_factor, 0.0, 65535.0));
        new_ramp[1][i] = static_cast<uint16_t>(std::clamp(sr->ramp[1][i] * g_factor, 0.0, 65535.0));
        new_ramp[2][i] = static_cast<uint16_t>(std::clamp(sr->ramp[2][i] * b_factor, 0.0, 65535.0));
    }

    BOOL ok = SetDeviceGammaRamp(hdc, new_ramp);

    if (!ok) {
        return Result::failure("SetDeviceGammaRamp failed");
    }

    sr->night_light_enabled = true;
    sr->temperature_kelvin = temperature_kelvin;
    return Result::success();
}

void WinGamma::restore_all() {
    for (auto& sr : saved_ramps_) {
        if (sr.has_original && sr.night_light_enabled) {
            DisplayDC dc(sr.device_name);
            if (dc.hdc) {
                SetDeviceGammaRamp(dc.hdc, sr.ramp);
            }
            sr.night_light_enabled = false;
        }
    }
}

} // namespace brodisplays
