#include "check.h"
#include "linux/private_session.h"
#include "brodisplays/display_service.h"

#include <cstdio>
#include <string>

// Night light under GNOME: gnome-settings-daemon's colour plugin, driven
// through its GSettings schema, with mutter deciding whether the outputs can
// show it. The session here is a private headless mutter plus gsd-color on a
// private bus. A headless mutter's virtual monitor has no gamma, and mutter
// says so (NightLightSupported=false): night light must be reported
// unsupported, refused with a reason, and leave the settings untouched. (A
// real GNOME session with gamma-capable outputs is not available here.)

namespace {

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string mutter_supported() {
    return trimmed(bdtest::run("busctl --user get-property org.gnome.Mutter.DisplayConfig "
                               "/org/gnome/Mutter/DisplayConfig org.gnome.Mutter.DisplayConfig NightLightSupported"));
}

std::string setting(const char* key) {
    return trimmed(bdtest::run(std::string("gsettings get org.gnome.settings-daemon.plugins.color ") + key));
}

} // namespace

int main() {
    const char* name = "test_linux_night_light_gnome";
    bstest::require_mutation_opt_in(name, "This test (on a private headless mutter it starts)");
    std::printf("[%s] night light through GNOME on a private headless mutter\n", name);
    // /usr/lib on Arch, /usr/libexec on Debian, Ubuntu and Fedora.
    const char* gsd = "/usr/lib/gsd-color";
    if (!bdtest::have_program(gsd)) gsd = "/usr/libexec/gsd-color";
    if (!bdtest::have_program("mutter") || !bdtest::have_program(gsd) || !bdtest::have_program("busctl") ||
        !bdtest::have_program("gsettings")) {
        bstest::skip(name, "mutter, gsd-color, busctl or gsettings is not installed");
    }

    bdtest::TempDir dir("nlgnome");
    REQUIRE(!dir.path().empty());
    bdtest::isolate_environment(dir);
    bdtest::Process bus = bdtest::start_bus(dir);
    REQUIRE(bus.running());
    setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);

    bdtest::Process mutter = bdtest::start_wayland(
        dir, {"mutter", "--headless", "--wayland", "--no-x11", "--virtual-monitor", "1280x800",
              "--wayland-display", "wl-mutter"},
        "wl-mutter", "mutter.log");
    if (!mutter.running()) bstest::skip(name, "mutter would not start headless");
    REQUIRE(bdtest::wait_for([] { return !mutter_supported().empty(); }, std::chrono::milliseconds(10000)));
    bdtest::Process gsd_color({gsd}, dir.path() + "/gsd-color.log");
    REQUIRE(bdtest::wait_for(
        [] {
            return bdtest::run("busctl --user status org.gnome.SettingsDaemon.Color >/dev/null && echo up")
                       .find("up") != std::string::npos;
        },
        std::chrono::milliseconds(10000)));
    std::printf("mutter NightLightSupported: %s\n", mutter_supported().c_str());
    if (mutter_supported() != "b false") {
        bstest::skip(name, "this mutter offers night light on its virtual monitor; the unsupported path cannot be "
                           "exercised (" + mutter_supported() + ")");
    }

    const std::string enabled_before = setting("night-light-enabled");
    const std::string temp_before = setting("night-light-temperature");
    const std::string auto_before = setting("night-light-schedule-automatic");
    REQUIRE(!enabled_before.empty());

    {
        auto service = brodisplays::DisplayService::create();
        REQUIRE(service != nullptr);
        auto snap = service->snapshot();
        REQUIRE(!snap.displays.empty());
        for (const auto& d : snap.displays) {
            CHECK(!d.night_light.supported);
            CHECK(!d.night_light.enabled);
        }
        auto res = service->set_night_light(true, 3800);
        std::printf("set_night_light(on, 3800K): %s\n", res.ok ? "ok" : res.error.c_str());
        CHECK(!res.ok);
        CHECK(res.error.find("GNOME") != std::string::npos);
    }
    CHECK_EQ(setting("night-light-enabled"), enabled_before);
    CHECK_EQ(setting("night-light-temperature"), temp_before);
    CHECK_EQ(setting("night-light-schedule-automatic"), auto_before);

    gsd_color.stop();
    mutter.stop();
    bus.stop();
    return bstest::finish(name);
}
