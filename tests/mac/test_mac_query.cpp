#include "check.h"
#include "brodisplays/display_service.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <regex>
#include <string>

// Oracle: system_profiler's JSON display list, matched to our displays by
// CGDirectDisplayID (_spdisplays_displayID). It reports the current pixel
// size, the desktop size in points with the refresh rate, main, sleep and
// mirror state.

namespace {

struct PipeCloser {
    void operator()(FILE* fp) const {
        if (fp) pclose(fp);
    }
};

std::string run_cmd(const char* cmd) {
    std::array<char, 512> buffer;
    std::string result;
    std::unique_ptr<FILE, PipeCloser> pipe(popen(cmd, "r"));
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// The JSON object that carries "_spdisplays_displayID" : "<id>".
std::string profiler_block(const std::string& json, const std::string& id) {
    const std::string key = "\"_spdisplays_displayID\" : \"" + id + "\"";
    size_t at = json.find(key);
    if (at == std::string::npos) return "";
    size_t open = json.rfind('{', at);
    size_t close = json.find('}', at);
    if (open == std::string::npos || close == std::string::npos) return "";
    return json.substr(open, close - open);
}

std::string field(const std::string& block, const std::string& name) {
    std::smatch m;
    std::regex re("\"" + name + "\" : \"([^\"]*)\"");
    return std::regex_search(block, m, re) ? m[1].str() : std::string();
}

} // namespace

int main() {
    std::printf("[test_mac_query] Starting macOS display query verification...\n");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    std::printf("Enumerated %zu attached display(s)\n", snap.displays.size());
    CHECK(snap.displays.size() >= 1);

    const auto* primary = snap.primary_display();
    REQUIRE(primary != nullptr);
    CHECK(primary->is_primary);
    std::printf("Primary display ID: %s, Name: %s\n", primary->id.c_str(), primary->name.c_str());

    std::string json = run_cmd("system_profiler -json SPDisplaysDataType 2>/dev/null");
    REQUIRE(!json.empty());
    for (const auto& d : snap.displays) {
        std::printf("Display %s '%s': %ux%u px @ %.2f Hz, %ux%u pt at %d,%d, scale %.2f, active %d, mirror_of '%s'\n",
                    d.id.c_str(), d.name.c_str(), d.current_mode.width, d.current_mode.height,
                    d.current_mode.refresh_rate, d.geometry.width, d.geometry.height, d.geometry.x, d.geometry.y,
                    d.scale.factor, d.is_active, d.mirror_of.c_str());
        CHECK(!d.id.empty());
        CHECK(!d.name.empty());
        CHECK_EQ(d.device_name, d.id);
        CHECK(d.current_mode.width > 0);
        CHECK(d.current_mode.height > 0);
        CHECK(!d.available_modes.empty());

        std::string block = profiler_block(json, d.id);
        CHECK(!block.empty());
        if (block.empty()) continue;
        const std::string pixels = field(block, "_spdisplays_pixels");
        const std::string resolution = field(block, "_spdisplays_resolution");
        std::printf("  system_profiler: pixels '%s', resolution '%s'\n", pixels.c_str(), resolution.c_str());
        CHECK_EQ(pixels, std::to_string(d.current_mode.width) + " x " + std::to_string(d.current_mode.height));
        std::smatch m;
        if (std::regex_search(resolution, m, std::regex("(\\d+) x (\\d+) @ ([0-9.]+)Hz"))) {
            CHECK_EQ(std::stoul(m[1].str()), static_cast<unsigned long>(d.geometry.width));
            CHECK_EQ(std::stoul(m[2].str()), static_cast<unsigned long>(d.geometry.height));
            CHECK(std::abs(std::stod(m[3].str()) - d.current_mode.refresh_rate) < 0.5);
        } else {
            CHECK(!"_spdisplays_resolution not parsed");
        }
        CHECK_EQ(field(block, "spdisplays_main") == "spdisplays_yes", d.is_primary);
        CHECK_EQ(field(block, "spdisplays_asleep") == "spdisplays_yes", !d.is_active);
        const std::string mirror = field(block, "spdisplays_mirror");
        if (mirror == "spdisplays_off") CHECK(d.mirror_of.empty());
    }

    return bstest::finish("test_mac_query");
}
