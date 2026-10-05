#include "check.h"
#include "linux/private_session.h"
#include "brodisplays/display_service.h"

#include <cstdio>
#include <string>

// Night light under KDE Plasma: KWin's own night light, driven through
// kwinrc's [NightColor] group plus the KConfig change notification, the way
// System Settings does it. KWin here is a private virtual kwin_wayland on a
// private session bus, with its own config home; the oracle is KWin's
// org.kde.KWin.NightLight D-Bus interface and the kwinrc it reads.

namespace {

std::string kwin_prop(const char* prop) {
    std::string out = bdtest::run(std::string("busctl --user get-property org.kde.KWin /org/kde/KWin/NightLight "
                                              "org.kde.KWin.NightLight ") +
                                  prop);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

bool has_line(const std::string& text, const std::string& line) {
    size_t pos = 0;
    while ((pos = text.find(line, pos)) != std::string::npos) {
        const bool starts = pos == 0 || text[pos - 1] == '\n';
        const size_t end = pos + line.size();
        const bool ends = end == text.size() || text[end] == '\n';
        if (starts && ends) return true;
        pos = end;
    }
    return false;
}

} // namespace

int main() {
    const char* name = "test_linux_night_light_kwin";
    bstest::require_mutation_opt_in(name, "This test (on a private virtual KWin it starts)");
    std::printf("[%s] night light through KWin on a private virtual kwin_wayland\n", name);
    if (!bdtest::have_program("kwin_wayland") || !bdtest::have_program("busctl")) {
        bstest::skip(name, "kwin_wayland or busctl is not installed");
    }

    bdtest::TempDir dir("nlkwin");
    REQUIRE(!dir.path().empty());
    bdtest::isolate_environment(dir);
    bdtest::Process bus = bdtest::start_bus(dir);
    REQUIRE(bus.running());

    // The user's own settings that must survive: another group, and their
    // night light schedule mode.
    const std::string kwinrc = dir.path() + "/config/kwinrc";
    REQUIRE(bdtest::write_file(kwinrc, "[Windows]\nFocusPolicy=ClickToFocus\n\n[NightColor]\nMode=Times\n"));

    setenv("XDG_CURRENT_DESKTOP", "KDE", 1);
    bdtest::Process kwin = bdtest::start_wayland(
        dir, {"kwin_wayland", "--virtual", "--socket", "wl-kwin", "--width", "1280", "--height", "800",
              "--no-lockscreen", "--no-global-shortcuts"},
        "wl-kwin", "kwin.log");
    if (!kwin.running()) {
        bstest::skip(name, "kwin_wayland would not start virtually:\n" + bdtest::log_tail(dir.path() + "/kwin.log"));
    }
    REQUIRE(bdtest::wait_for([] { return kwin_prop("available") == "b true"; }, std::chrono::milliseconds(10000)));
    std::printf("KWin night light: available=%s running=%s\n", kwin_prop("available").c_str(),
                kwin_prop("running").c_str());
    CHECK_EQ(kwin_prop("running"), std::string("b false"));

    {
        auto service = brodisplays::DisplayService::create();
        REQUIRE(service != nullptr);
        auto snap = service->snapshot();
        REQUIRE(!snap.displays.empty());
        for (const auto& d : snap.displays) {
            CHECK(d.night_light.supported);
            CHECK(!d.night_light.enabled);
        }

        auto res = service->set_night_light(true, 3700);
        std::printf("set_night_light(on, 3700K): %s\n", res.ok ? "ok" : res.error.c_str());
        CHECK(res.ok);
        CHECK_EQ(kwin_prop("enabled"), std::string("b true"));
        CHECK(bdtest::wait_for([] { return kwin_prop("running") == "b true"; }, std::chrono::milliseconds(3000)));
        CHECK_EQ(kwin_prop("targetTemperature"), std::string("u 3700"));
        CHECK(bdtest::wait_for([] { return kwin_prop("currentTemperature") == "u 3700"; },
                               std::chrono::milliseconds(8000)));
        std::string rc = bdtest::read_file(kwinrc);
        CHECK(has_line(rc, "Active=true"));
        CHECK(has_line(rc, "Mode=Constant"));
        CHECK(has_line(rc, "NightTemperature=3700"));
        CHECK(has_line(rc, "FocusPolicy=ClickToFocus"));
        snap = service->snapshot();
        for (const auto& d : snap.displays) CHECK(d.night_light.enabled);

        res = service->set_night_light(false, 0);
        std::printf("set_night_light(off): %s\n", res.ok ? "ok" : res.error.c_str());
        CHECK(res.ok);
        CHECK_EQ(kwin_prop("enabled"), std::string("b false"));
        CHECK(bdtest::wait_for([] { return kwin_prop("running") == "b false"; }, std::chrono::milliseconds(3000)));
        rc = bdtest::read_file(kwinrc);
        CHECK(has_line(rc, "Active=false"));
        CHECK(has_line(rc, "Mode=Times"));
        CHECK(has_line(rc, "FocusPolicy=ClickToFocus"));
        snap = service->snapshot();
        for (const auto& d : snap.displays) CHECK(!d.night_light.enabled);
    }

    kwin.stop();
    bus.stop();
    return bstest::finish(name);
}
