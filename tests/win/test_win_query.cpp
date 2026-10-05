#include "check.h"
#include "brodisplays/display_service.h"

#include <array>
#include <cstdio>
#include <memory>
#include <string>

namespace {

std::string run_cmd(const char* cmd) {
    std::array<char, 256> buffer;
    std::string result;
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd, "r"), _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
#endif
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    // Trim
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

} // namespace

int main() {
    std::printf("[test_win_query] Starting Windows display query verification...\n");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    std::printf("Enumerated %zu attached display(s)\n", snap.displays.size());
    CHECK(snap.displays.size() >= 1);

    const auto* primary = snap.primary_display();
    REQUIRE(primary != nullptr);
    std::printf("Primary display ID: %s\n", primary->id.c_str());
    std::printf("Primary display Name: %s\n", primary->name.c_str());
    std::printf("Primary resolution: %ux%u @ %.1f Hz\n",
        primary->current_mode.width, primary->current_mode.height, primary->current_mode.refresh_rate);
    std::printf("Scale factor: %.2f (DPI %d)\n", primary->scale.factor, primary->scale.dpi);

    CHECK(!primary->id.empty());
    CHECK(!primary->name.empty());
    CHECK(primary->current_mode.width > 0);
    CHECK(primary->current_mode.height > 0);
    CHECK(primary->current_mode.refresh_rate > 0.0);
    CHECK(primary->scale.factor >= 1.0);
    CHECK(primary->scale.dpi >= 96);
    CHECK(!primary->available_modes.empty());

    // Verify against OS command line oracle: Get-CimInstance Win32_VideoController
    std::string oracle_w_str = run_cmd(
        "powershell.exe -NoProfile -Command \"(Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentHorizontalResolution -gt 0 } | Select-Object -First 1).CurrentHorizontalResolution\"");
    std::string oracle_h_str = run_cmd(
        "powershell.exe -NoProfile -Command \"(Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentVerticalResolution -gt 0 } | Select-Object -First 1).CurrentVerticalResolution\"");

    if (!oracle_w_str.empty() && !oracle_h_str.empty()) {
        try {
            uint32_t oracle_w = static_cast<uint32_t>(std::stoul(oracle_w_str));
            uint32_t oracle_h = static_cast<uint32_t>(std::stoul(oracle_h_str));
            std::printf("OS Oracle reports: %ux%u\n", oracle_w, oracle_h);

            // One of our connected displays should match the active video controller resolution
            bool matched = false;
            for (const auto& d : snap.displays) {
                if (d.current_mode.width == oracle_w && d.current_mode.height == oracle_h) {
                    matched = true;
                    break;
                }
            }
            CHECK(matched);
        } catch (...) {
            std::fprintf(stderr, "Could not parse oracle output: '%s' x '%s'\n",
                oracle_w_str.c_str(), oracle_h_str.c_str());
        }
    }

    return bstest::finish("test_win_query");
}
