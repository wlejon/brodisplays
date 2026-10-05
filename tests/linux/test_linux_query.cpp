#include "check.h"
#include "brodisplays/display_service.h"

#include <array>
#include <chrono>
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

    // Creating, querying and destroying the service talks to whatever display
    // server the environment names. None of it may block indefinitely: every
    // compositor exchange is bounded, so the whole run must stay well inside
    // the ctest timeout even when protocols are missing.
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::string err;
        auto service = brodisplays::DisplayService::create({}, &err);
        REQUIRE(service != nullptr);

        auto snap = service->snapshot();
        std::printf("Enumerated %zu attached display(s)\n", snap.displays.size());
        if (snap.displays.empty()) {
            bstest::skip("test_linux_query", "no display server and no connected DRM connector on this machine");
        }

        const auto* primary = snap.primary_display();
        REQUIRE(primary != nullptr);
        std::printf("Primary display ID: %s, Name: %s, Backend: %s\n", primary->id.c_str(),
                    primary->name.c_str(), primary->adapter_name.c_str());
        std::printf("Resolution: %ux%u @ %.2f Hz at %d,%d\n", primary->current_mode.width,
                    primary->current_mode.height, primary->current_mode.refresh_rate,
                    primary->geometry.x, primary->geometry.y);

        CHECK(!primary->id.empty());
        CHECK(primary->current_mode.width > 0);
        CHECK(primary->current_mode.height > 0);
        CHECK(!primary->available_modes.empty());

        // xrandr is an independent oracle under X11 and under Wayland (via
        // Xwayland, which mirrors the compositor's outputs, though under its
        // own output names). Every enumerated display's desktop rectangle must
        // appear in it; under X11 the output names must match too.
        std::string xrandr_out = run_cmd("timeout 5 xrandr 2>/dev/null | grep ' connected'");
        const bool is_x11 = primary->adapter_name.find("X11") != std::string::npos;
        const bool is_wayland = primary->adapter_name.find("Wayland") != std::string::npos;
        if (!xrandr_out.empty() && (is_x11 || is_wayland)) {
            std::printf("xrandr oracle:\n%s", xrandr_out.c_str());
            for (const auto& d : snap.displays) {
                std::string rect = std::to_string(d.geometry.width) + "x" + std::to_string(d.geometry.height) +
                                   "+" + std::to_string(d.geometry.x) + "+" + std::to_string(d.geometry.y);
                bool rect_found = xrandr_out.find(rect) != std::string::npos;
                if (!rect_found) std::printf("  %s (%s) not in xrandr output\n", d.id.c_str(), rect.c_str());
                CHECK(rect_found);
                if (is_x11) CHECK(xrandr_out.find(d.id + " connected") != std::string::npos);
            }
        } else {
            std::printf("Note: no xrandr oracle for backend '%s'\n", primary->adapter_name.c_str());
        }

        // Mode changes need a protocol many compositors lack; the service must
        // answer (success or an honest error) rather than hang. Re-applying the
        // current mode is a no-op on the display, so this is safe by default.
        brodisplays::DisplayConfigChange same;
        same.display_id = primary->id;
        same.width = primary->current_mode.width;
        same.height = primary->current_mode.height;
        same.refresh_rate = primary->current_mode.refresh_rate;
        if (bstest::mutation_opted_in()) {
            auto res = service->apply_configuration(same);
            std::printf("Re-apply current mode: %s%s\n", res.ok ? "ok" : "refused: ", res.error.c_str());
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    std::printf("Create/query/destroy took %lld ms\n", static_cast<long long>(elapsed.count()));
    CHECK(elapsed < std::chrono::seconds(15));

    return bstest::finish("test_linux_query");
}
