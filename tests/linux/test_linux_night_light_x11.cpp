#include "check.h"
#include "linux/private_session.h"
#include "brodisplays/display_service.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <xcb/randr.h>
#include <xcb/xcb.h>

// Night light on a plain X server (no desktop owning the ramps): RandR CRTC
// gamma. The server is a private Xorg on the dummy driver (Xvfb accepts
// ramps but does not keep them); the oracle reads the active CRTC's ramps
// back over a separate connection.

namespace {

struct Ramp {
    std::vector<uint16_t> r, g, b;
    bool ok = false;
};

Ramp read_ramp() {
    Ramp out;
    xcb_connection_t* c = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(c)) {
        xcb_disconnect(c);
        return out;
    }
    xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    auto* res = xcb_randr_get_screen_resources_current_reply(
        c, xcb_randr_get_screen_resources_current(c, root), nullptr);
    xcb_randr_crtc_t crtc = XCB_NONE;
    for (int i = 0; res && i < xcb_randr_get_screen_resources_current_crtcs_length(res) && crtc == XCB_NONE; ++i) {
        xcb_randr_crtc_t id = xcb_randr_get_screen_resources_current_crtcs(res)[i];
        auto* info = xcb_randr_get_crtc_info_reply(c, xcb_randr_get_crtc_info(c, id, XCB_CURRENT_TIME), nullptr);
        if (info && info->mode != XCB_NONE) crtc = id;
        free(info);
    }
    if (crtc != XCB_NONE) {
        auto* g = xcb_randr_get_crtc_gamma_reply(c, xcb_randr_get_crtc_gamma(c, crtc), nullptr);
        if (g && g->size > 0) {
            const uint16_t* r = xcb_randr_get_crtc_gamma_red(g);
            const uint16_t* gr = xcb_randr_get_crtc_gamma_green(g);
            const uint16_t* b = xcb_randr_get_crtc_gamma_blue(g);
            out.r.assign(r, r + g->size);
            out.g.assign(gr, gr + g->size);
            out.b.assign(b, b + g->size);
            out.ok = true;
        }
        free(g);
    }
    free(res);
    xcb_disconnect(c);
    return out;
}

bool is_warm(const Ramp& ramp) {
    // A warm white point: red at full, blue clearly attenuated at the top.
    return ramp.ok && ramp.b.back() < ramp.r.back() * 0.9 && ramp.g.back() < ramp.r.back();
}

bool same(const Ramp& a, const Ramp& b) {
    return a.ok && b.ok && a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

int main() {
    const char* name = "test_linux_night_light_x11";
    bstest::require_mutation_opt_in(name, "This test (on a private Xorg it starts)");
    std::printf("[%s] night light through RandR CRTC gamma on a private Xorg (dummy driver)\n", name);

    bdtest::TempDir dir("nlx11");
    REQUIRE(!dir.path().empty());
    bdtest::isolate_environment(dir);
    bdtest::Process xorg = bdtest::start_xorg_dummy(dir);
    if (!xorg.running()) bstest::skip(name, "Xorg with the dummy video driver is not installed or would not start");

    const Ramp original = read_ramp();
    REQUIRE(original.ok);
    CHECK(!is_warm(original));

    {
        auto service = brodisplays::DisplayService::create();
        REQUIRE(service != nullptr);
        auto snap = service->snapshot();
        REQUIRE(!snap.displays.empty());
        const auto* d = snap.primary_display();
        REQUIRE(d != nullptr);
        CHECK(d->night_light.supported);
        CHECK(!d->night_light.enabled);

        auto res = service->set_night_light(true, 3400);
        std::printf("set_night_light(on, 3400K): %s\n", res.ok ? "ok" : res.error.c_str());
        CHECK(res.ok);
        Ramp warm = read_ramp();
        CHECK(is_warm(warm));
        snap = service->snapshot();
        CHECK(snap.primary_display() && snap.primary_display()->night_light.enabled);
        CHECK(snap.primary_display() && snap.primary_display()->night_light.temperature_kelvin == 3400u);

        // A second temperature replaces the first rather than compounding.
        res = service->set_night_light(true, 5000);
        CHECK(res.ok);
        Ramp milder = read_ramp();
        CHECK(is_warm(milder));
        CHECK(milder.b.back() > warm.b.back());

        res = service->set_night_light(false, 0);
        CHECK(res.ok);
        CHECK(same(read_ramp(), original));
        snap = service->snapshot();
        CHECK(snap.primary_display() && !snap.primary_display()->night_light.enabled);

        // Left on when the service goes away: the ramps it changed go back.
        res = service->set_night_light(true, 3000);
        CHECK(res.ok);
        CHECK(is_warm(read_ramp()));
    }
    CHECK(same(read_ramp(), original));

    return bstest::finish(name);
}
