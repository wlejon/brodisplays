#include "common/color_temperature.h"

#include <algorithm>
#include <cmath>

namespace brodisplays {

void kelvin_to_rgb(uint32_t kelvin, double& r, double& g, double& b) {
    double temp = static_cast<double>(std::clamp(kelvin, 1000u, 10000u)) / 100.0;

    if (temp <= 66.0) {
        r = 1.0;
    } else {
        r = 329.698727446 * std::pow(temp - 60.0, -0.1332047592);
        r = std::clamp(r / 255.0, 0.0, 1.0);
    }

    if (temp <= 66.0) {
        g = 99.4708025861 * std::log(temp) - 161.1195681661;
    } else {
        g = 288.1221695283 * std::pow(temp - 60.0, -0.0755148492);
    }
    g = std::clamp(g / 255.0, 0.0, 1.0);

    if (temp >= 66.0) {
        b = 1.0;
    } else if (temp <= 19.0) {
        b = 0.0;
    } else {
        b = 138.5177312231 * std::log(temp - 10.0) - 305.0447927307;
        b = std::clamp(b / 255.0, 0.0, 1.0);
    }
}

} // namespace brodisplays
