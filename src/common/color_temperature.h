#pragma once

#include <cstdint>

namespace brodisplays {

// Relative channel gains (0..1) of a black body at `kelvin` (clamped to
// 1000..10000 K), white at about 6500 K; Tanner Helland's fit, the curve the
// gamma-ramp night lights multiply their ramps by.
void kelvin_to_rgb(uint32_t kelvin, double& r, double& g, double& b);

} // namespace brodisplays
