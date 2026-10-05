#include "check.h"
#include "linux/private_session.h"
#include "brodisplays/display_service.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <wayland-client.h>
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

// Night light on a wlroots compositor: wlr-gamma-control. The compositor is a
// private headless sway with two outputs. The oracle is the protocol's own
// exclusivity: while a client holds an output's gamma control, the
// compositor fails anyone else's, and it hands the output back (restoring
// its ramp) once the holder lets go or disconnects.

namespace {

struct Probe {
    wl_display* display = nullptr;
    zwlr_gamma_control_manager_v1* manager = nullptr;
    std::vector<wl_output*> outputs;

    static void global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
        auto* p = static_cast<Probe*>(data);
        if (std::strcmp(iface, wl_output_interface.name) == 0) {
            p->outputs.push_back(static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 1)));
        } else if (std::strcmp(iface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
            p->manager = static_cast<zwlr_gamma_control_manager_v1*>(
                wl_registry_bind(reg, name, &zwlr_gamma_control_manager_v1_interface, version < 1 ? version : 1));
        }
    }
    static void global_remove(void*, wl_registry*, uint32_t) {}
};

struct ControlState {
    uint32_t size = 0;
    bool failed = false;
};

void on_size(void* data, zwlr_gamma_control_v1*, uint32_t size) {
    static_cast<ControlState*>(data)->size = size;
}
void on_failed(void* data, zwlr_gamma_control_v1*) {
    static_cast<ControlState*>(data)->failed = true;
}

// How many outputs another client could take gamma control of right now;
// -1 when the compositor cannot be reached.
int free_outputs(size_t* total = nullptr) {
    static const wl_registry_listener kRegistry = {Probe::global, Probe::global_remove};
    static const zwlr_gamma_control_v1_listener kControl = {on_size, on_failed};
    Probe p;
    p.display = wl_display_connect(nullptr);
    if (!p.display) return -1;
    wl_registry* reg = wl_display_get_registry(p.display);
    wl_registry_add_listener(reg, &kRegistry, &p);
    wl_display_roundtrip(p.display);
    int free_count = -1;
    if (p.manager) {
        std::vector<ControlState> states(p.outputs.size());
        std::vector<zwlr_gamma_control_v1*> controls;
        for (size_t i = 0; i < p.outputs.size(); ++i) {
            auto* c = zwlr_gamma_control_manager_v1_get_gamma_control(p.manager, p.outputs[i]);
            zwlr_gamma_control_v1_add_listener(c, &kControl, &states[i]);
            controls.push_back(c);
        }
        wl_display_roundtrip(p.display);
        free_count = 0;
        for (const auto& s : states) {
            if (!s.failed && s.size > 0) ++free_count;
        }
        for (auto* c : controls) zwlr_gamma_control_v1_destroy(c);
        zwlr_gamma_control_manager_v1_destroy(p.manager);
    }
    if (total) *total = p.outputs.size();
    for (auto* o : p.outputs) wl_output_destroy(o);
    wl_registry_destroy(reg);
    wl_display_roundtrip(p.display);
    wl_display_disconnect(p.display);
    return free_count;
}

} // namespace

int main() {
    const char* name = "test_linux_night_light_wlr";
    bstest::require_mutation_opt_in(name, "This test (on a private headless sway it starts)");
    std::printf("[%s] night light through wlr-gamma-control on a private headless sway\n", name);

    bdtest::TempDir dir("nlwlr");
    REQUIRE(!dir.path().empty());
    bdtest::isolate_environment(dir);
    const std::string config = dir.path() + "/sway.conf";
    REQUIRE(bdtest::write_file(config, "# brodisplays test: no bar, no clients\n"));
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "2", 1);
    setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
    bdtest::Process sway = bdtest::start_wayland(dir, {"sway", "-c", config}, "", "sway.log");
    if (!bdtest::have_program("sway")) bstest::skip(name, "sway is not installed");
    if (!sway.running()) {
        bstest::skip(name, "sway would not start headless:\n" + bdtest::log_tail(dir.path() + "/sway.log"));
    }

    size_t outputs = 0;
    int usable = -1;
    REQUIRE(bdtest::wait_for([&] { return (usable = free_outputs(&outputs)) >= 0 && outputs > 0; },
                             std::chrono::milliseconds(5000)));
    std::printf("sway on %s: %zu output(s), %d with a gamma ramp\n", std::getenv("WAYLAND_DISPLAY"), outputs,
                usable);

    if (usable == 0) {
        // Outputs without a ramp (the headless backend has none): the
        // compositor refuses every control, and night light must say so.
        auto service = brodisplays::DisplayService::create();
        REQUIRE(service != nullptr);
        auto snap = service->snapshot();
        CHECK_EQ(snap.displays.size(), outputs);
        for (const auto& d : snap.displays) {
            CHECK(!d.night_light.supported);
            CHECK(!d.night_light.enabled);
        }
        auto res = service->set_night_light(true, 3600);
        std::printf("set_night_light on outputs without gamma: %s\n", res.ok ? "ok" : res.error.c_str());
        CHECK(!res.ok);
        CHECK_EQ(free_outputs(), 0);
        return bstest::finish(name);
    }
    CHECK_EQ(usable, static_cast<int>(outputs));

    {
        auto service = brodisplays::DisplayService::create();
        REQUIRE(service != nullptr);
        auto snap = service->snapshot();
        CHECK_EQ(snap.displays.size(), outputs);
        for (const auto& d : snap.displays) {
            CHECK(d.night_light.supported);
            CHECK(!d.night_light.enabled);
        }

        auto res = service->set_night_light(true, 3600);
        std::printf("set_night_light(on, 3600K): %s\n", res.ok ? "ok" : res.error.c_str());
        CHECK(res.ok);
        CHECK_EQ(free_outputs(), 0);
        snap = service->snapshot();
        for (const auto& d : snap.displays) {
            CHECK(d.night_light.enabled);
            CHECK_EQ(d.night_light.temperature_kelvin, 3600u);
        }

        res = service->set_night_light(true, 4800);
        CHECK(res.ok);
        CHECK_EQ(free_outputs(), 0);
        snap = service->snapshot();
        CHECK(snap.primary_display() && snap.primary_display()->night_light.temperature_kelvin == 4800u);

        res = service->set_night_light(false, 0);
        CHECK(res.ok);
        CHECK_EQ(free_outputs(), static_cast<int>(outputs));
        snap = service->snapshot();
        for (const auto& d : snap.displays) CHECK(!d.night_light.enabled);

        // Another client already holding the outputs: refused, said so.
        {
            static const wl_registry_listener kRegistry = {Probe::global, Probe::global_remove};
            static const zwlr_gamma_control_v1_listener kControl = {on_size, on_failed};
            Probe holder;
            holder.display = wl_display_connect(nullptr);
            REQUIRE(holder.display != nullptr);
            wl_registry* reg = wl_display_get_registry(holder.display);
            wl_registry_add_listener(reg, &kRegistry, &holder);
            wl_display_roundtrip(holder.display);
            REQUIRE(holder.manager != nullptr);
            std::vector<ControlState> states(holder.outputs.size());
            for (size_t i = 0; i < holder.outputs.size(); ++i) {
                auto* c = zwlr_gamma_control_manager_v1_get_gamma_control(holder.manager, holder.outputs[i]);
                zwlr_gamma_control_v1_add_listener(c, &kControl, &states[i]);
            }
            wl_display_roundtrip(holder.display);
            res = service->set_night_light(true, 3600);
            std::printf("with another holder: %s\n", res.ok ? "ok" : res.error.c_str());
            CHECK(!res.ok);
            snap = service->snapshot();
            for (const auto& d : snap.displays) CHECK(!d.night_light.enabled);
            wl_display_disconnect(holder.display);
        }
        CHECK(bdtest::wait_for([&] { return free_outputs() == static_cast<int>(outputs); },
                               std::chrono::milliseconds(2000)));

        // Left on when the service goes away: the outputs go back.
        res = service->set_night_light(true, 3000);
        CHECK(res.ok);
        CHECK_EQ(free_outputs(), 0);
    }
    CHECK(bdtest::wait_for([&] { return free_outputs() == static_cast<int>(outputs); },
                           std::chrono::milliseconds(2000)));

    return bstest::finish(name);
}
