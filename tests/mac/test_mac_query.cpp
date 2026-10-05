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
    std::printf("[test_mac_query] Starting macOS display query verification...\n");

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
    std::printf("Scale factor: %.2f (DPI %d)\n", primary->scale.factor, primary->scale.dpi);

    CHECK(!primary->id.empty());
    CHECK(!primary->name.empty());
    CHECK(primary->current_mode.width > 0);
    CHECK(primary->current_mode.height > 0);
    CHECK(!primary->available_modes.empty());

    // Compare against macOS system_profiler SPDisplaysDataType
    std::string profiler_out = run_cmd("system_profiler SPDisplaysDataType 2>/dev/null");
    if (!profiler_out.empty()) {
        std::printf("system_profiler oracle found output (%zu bytes)\n", profiler_out.size());
        bool found_name = (profiler_out.find(primary->name) != std::string::npos);
        if (found_name) {
            std::printf("Matched display name '%s' in system_profiler\n", primary->name.c_str());
            CHECK(found_name);
        } else {
            // Also check resolution string
            std::string res_pattern = std::to_string(primary->current_mode.width) + " x " +
                                      std::to_string(primary->current_mode.height);
            bool found_res = (profiler_out.find(res_pattern) != std::string::npos);
            CHECK(found_name || found_res);
        }
    }

    return bstest::finish("test_mac_query");
}
