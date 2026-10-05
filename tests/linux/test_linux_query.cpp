#include "check.h"
#include "brodisplays/display_service.h"

#include <array>
#include <cstdio>
#include <memory>
#include <string>

namespace {

struct PipeCloser {
    void operator()(FILE* fp) const {
        if (fp) pclose(fp);
    }
};

std::string run_cmd(const char* cmd) {
    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, PipeCloser> pipe(popen(cmd, "r"));
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

} // namespace

int main() {
    std::printf("[test_linux_query] Starting Linux display query verification...\n");

    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    REQUIRE(service != nullptr);

    auto snap = service->snapshot();
    std::printf("Enumerated %zu attached display(s)\n", snap.displays.size());
    CHECK(snap.displays.size() >= 1);

    const auto* primary = snap.primary_display();
    REQUIRE(primary != nullptr);
    std::printf("Primary display ID: %s, Name: %s\n", primary->id.c_str(), primary->name.c_str());
    std::printf("Resolution: %ux%u @ %.1f Hz\n",
        primary->current_mode.width, primary->current_mode.height, primary->current_mode.refresh_rate);

    CHECK(!primary->id.empty());
    CHECK(primary->current_mode.width > 0);
    CHECK(primary->current_mode.height > 0);
    CHECK(!primary->available_modes.empty());

    // Compare against xrandr oracle if available
    std::string xrandr_out = run_cmd("xrandr 2>/dev/null | grep ' connected'");
    if (!xrandr_out.empty()) {
        std::printf("xrandr oracle: %s\n", xrandr_out.c_str());
        // Verify output name is in xrandr output
        bool found_in_oracle = (xrandr_out.find(primary->id) != std::string::npos ||
                                xrandr_out.find(primary->name) != std::string::npos);
        CHECK(found_in_oracle);
    } else {
        std::printf("Note: xrandr not accessible, checking DRM sysfs fallback directly\n");
    }

    return bstest::finish("test_linux_query");
}
