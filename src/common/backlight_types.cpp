#include "brodisplays/backlight.h"

#include <algorithm>
#include <cmath>

namespace brodisplays {

std::string_view to_string(BacklightType type) noexcept {
    switch (type) {
        case BacklightType::Raw:
            return "raw";
        case BacklightType::Platform:
            return "platform";
        case BacklightType::Firmware:
            return "firmware";
        case BacklightType::Unknown:
        default:
            return "unknown";
    }
}

BacklightType backlight_type_from_string(std::string_view str) noexcept {
    if (str == "raw") return BacklightType::Raw;
    if (str == "platform") return BacklightType::Platform;
    if (str == "firmware") return BacklightType::Firmware;
    return BacklightType::Unknown;
}

double BacklightDevice::to_normalized(uint32_t raw, BrightnessCurve curve) const noexcept {
    return calculate_normalized(raw, min_brightness, max_brightness, curve);
}

uint32_t BacklightDevice::to_raw(double normalized, BrightnessCurve curve) const noexcept {
    return calculate_raw(normalized, min_brightness, max_brightness, curve);
}

double BacklightDevice::calculate_normalized(uint32_t raw, uint32_t min_b, uint32_t max_b,
                                            BrightnessCurve curve) noexcept {
    if (max_b <= min_b) {
        return max_b == 0 ? 0.0 : 1.0;
    }

    const uint32_t clamped_raw = std::clamp(raw, min_b, max_b);
    const double linear = static_cast<double>(clamped_raw - min_b) /
                          static_cast<double>(max_b - min_b);

    switch (curve) {
        case BrightnessCurve::Linear:
            return std::clamp(linear, 0.0, 1.0);

        case BrightnessCurve::Perceptual:
        default: {
            if (linear <= 0.0) return 0.0;
            if (linear >= 1.0) return 1.0;
            // Gamma 2.2 perceptual curve: human eye perceives luminance logarithmically,
            // so normalized perceptual brightness relates to physical luminance as Y^(1/gamma).
            constexpr double kInvGamma = 1.0 / 2.2;
            const double perceptual = std::pow(linear, kInvGamma);
            return std::clamp(perceptual, 0.0, 1.0);
        }
    }
}

uint32_t BacklightDevice::calculate_raw(double normalized, uint32_t min_b, uint32_t max_b,
                                       BrightnessCurve curve) noexcept {
    if (max_b <= min_b) {
        return min_b;
    }

    const double clamped_norm = std::clamp(normalized, 0.0, 1.0);
    const double range = static_cast<double>(max_b - min_b);

    double target_linear = clamped_norm;
    switch (curve) {
        case BrightnessCurve::Linear:
            target_linear = clamped_norm;
            break;

        case BrightnessCurve::Perceptual:
        default: {
            if (clamped_norm <= 0.0) {
                target_linear = 0.0;
            } else if (clamped_norm >= 1.0) {
                target_linear = 1.0;
            } else {
                constexpr double kGamma = 2.2;
                target_linear = std::pow(clamped_norm, kGamma);
            }
            break;
        }
    }

    const double raw_val = static_cast<double>(min_b) + std::round(target_linear * range);
    const double clamped_raw = std::clamp(raw_val, static_cast<double>(min_b), static_cast<double>(max_b));
    return static_cast<uint32_t>(clamped_raw);
}

#if !defined(__linux__)
std::unique_ptr<BacklightManager> BacklightManager::create() {
    return nullptr;
}
#endif

} // namespace brodisplays
