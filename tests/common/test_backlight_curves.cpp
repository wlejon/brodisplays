#include "check.h"
#include "brodisplays/backlight.h"

#include <cmath>

int main() {
    std::printf("[test_backlight_curves] Starting backlight curves verification...\n");

    // 1. BacklightType enum and conversions
    CHECK_EQ(brodisplays::to_string(brodisplays::BacklightType::Raw), "raw");
    CHECK_EQ(brodisplays::to_string(brodisplays::BacklightType::Platform), "platform");
    CHECK_EQ(brodisplays::to_string(brodisplays::BacklightType::Firmware), "firmware");
    CHECK_EQ(brodisplays::to_string(brodisplays::BacklightType::Unknown), "unknown");

    CHECK(brodisplays::backlight_type_from_string("raw") == brodisplays::BacklightType::Raw);
    CHECK(brodisplays::backlight_type_from_string("platform") == brodisplays::BacklightType::Platform);
    CHECK(brodisplays::backlight_type_from_string("firmware") == brodisplays::BacklightType::Firmware);
    CHECK(brodisplays::backlight_type_from_string("something_else") == brodisplays::BacklightType::Unknown);

    // 2. Linear curve tests
    const uint32_t min_b = 10;
    const uint32_t max_b = 210;

    // Boundaries
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(0.0, min_b, max_b, brodisplays::BrightnessCurve::Linear), 10u);
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(1.0, min_b, max_b, brodisplays::BrightnessCurve::Linear), 210u);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(10, min_b, max_b, brodisplays::BrightnessCurve::Linear) - 0.0) < 1e-6);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(210, min_b, max_b, brodisplays::BrightnessCurve::Linear) - 1.0) < 1e-6);

    // Midpoint
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(0.5, min_b, max_b, brodisplays::BrightnessCurve::Linear), 110u);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(110, min_b, max_b, brodisplays::BrightnessCurve::Linear) - 0.5) < 1e-6);

    // Clamping
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(-0.5, min_b, max_b, brodisplays::BrightnessCurve::Linear), 10u);
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(1.5, min_b, max_b, brodisplays::BrightnessCurve::Linear), 210u);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(5, min_b, max_b, brodisplays::BrightnessCurve::Linear) - 0.0) < 1e-6);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(300, min_b, max_b, brodisplays::BrightnessCurve::Linear) - 1.0) < 1e-6);

    // 3. Perceptual curve tests
    const uint32_t p_min = 0;
    const uint32_t p_max = 1000;

    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(0.0, p_min, p_max, brodisplays::BrightnessCurve::Perceptual), 0u);
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(1.0, p_min, p_max, brodisplays::BrightnessCurve::Perceptual), 1000u);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(0, p_min, p_max, brodisplays::BrightnessCurve::Perceptual) - 0.0) < 1e-6);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(1000, p_min, p_max, brodisplays::BrightnessCurve::Perceptual) - 1.0) < 1e-6);

    // Midpoint on perceptual curve must be significantly lower than linear midpoint (0.5^2.2 * 1000 ~ 218)
    uint32_t mid_raw = brodisplays::BacklightDevice::calculate_raw(0.5, p_min, p_max, brodisplays::BrightnessCurve::Perceptual);
    CHECK(mid_raw > 200u && mid_raw < 250u);

    // Round-trip check for mid_raw
    double mid_norm = brodisplays::BacklightDevice::calculate_normalized(mid_raw, p_min, p_max, brodisplays::BrightnessCurve::Perceptual);
    CHECK(std::abs(mid_norm - 0.5) < 0.02);

    // Strict monotonicity check for both curves
    uint32_t last_linear = 0;
    uint32_t last_perceptual = 0;
    for (int i = 0; i <= 100; ++i) {
        double n = static_cast<double>(i) / 100.0;
        uint32_t raw_l = brodisplays::BacklightDevice::calculate_raw(n, p_min, p_max, brodisplays::BrightnessCurve::Linear);
        uint32_t raw_p = brodisplays::BacklightDevice::calculate_raw(n, p_min, p_max, brodisplays::BrightnessCurve::Perceptual);

        if (i > 0) {
            CHECK(raw_l >= last_linear);
            CHECK(raw_p >= last_perceptual);
        }
        last_linear = raw_l;
        last_perceptual = raw_p;
    }

    // 4. Degenerate range tests (min >= max)
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(0.5, 100, 100, brodisplays::BrightnessCurve::Linear), 100u);
    CHECK_EQ(brodisplays::BacklightDevice::calculate_raw(0.5, 100, 100, brodisplays::BrightnessCurve::Perceptual), 100u);
    CHECK(std::abs(brodisplays::BacklightDevice::calculate_normalized(100, 100, 100, brodisplays::BrightnessCurve::Linear) - 1.0) < 1e-6);

    // 5. BacklightDevice member methods
    brodisplays::BacklightDevice dev;
    dev.name = "test_backlight";
    dev.sysfs_path = "/sys/class/backlight/test_backlight";
    dev.type = brodisplays::BacklightType::Raw;
    dev.min_brightness = 10;
    dev.max_brightness = 500;
    dev.current_brightness = 255;
    dev.associated_connector = "eDP-1";

    double dev_norm = dev.to_normalized(dev.current_brightness, brodisplays::BrightnessCurve::Linear);
    CHECK(dev_norm > 0.49 && dev_norm < 0.51);

    uint32_t dev_raw = dev.to_raw(dev_norm, brodisplays::BrightnessCurve::Linear);
    CHECK_EQ(dev_raw, 255u);

    return bstest::finish("test_backlight_curves");
}
