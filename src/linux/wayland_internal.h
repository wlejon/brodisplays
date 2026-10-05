#pragma once

// Protocol object records of WaylandBackend, shared by its translation units
// (wayland_backend.cpp: registry, outputs, output management;
// wayland_gamma.cpp: wlr-gamma-control night light).

#include <memory>
#include <string>
#include <vector>

#include <wayland-client.h>
#include "wayland_backend.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

namespace brodisplays {

// A gamma control the backend holds on one output while night light is on.
// The compositor keeps the ramp only as long as the control object lives.
struct WaylandBackend::GammaControl {
    zwlr_gamma_control_v1* control = nullptr;
    uint32_t size = 0;      // gamma_size event; 0 until it arrives
    bool failed = false;    // the compositor refused or revoked the control
};

struct WaylandBackend::OutputData {
    WaylandBackend* owner = nullptr;
    uint32_t id = 0;
    uint32_t version = 0;
    wl_output* output = nullptr;
    DisplayInfo info;
    std::vector<DisplayMode> pending_modes; // modes of the batch before `done`
    bool done = false;
    std::unique_ptr<GammaControl> gamma;
    // Whether the compositor gives this output a gamma ramp (found out by a
    // probe control): -1 not yet known, 0 no, 1 yes.
    int gamma_usable = -1;
};

struct WaylandBackend::ModeItem {
    HeadData* head = nullptr;
    zwlr_output_mode_v1* mode_obj = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    int32_t refresh_mhz = 0;
    bool preferred = false;
};

struct WaylandBackend::HeadData {
    WaylandBackend* owner = nullptr;
    zwlr_output_head_v1* head = nullptr;
    std::string name;
    std::string description;
    std::string make;
    std::string model;
    std::string serial_number;
    int32_t phys_w = 0;
    int32_t phys_h = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t transform = 0;
    double scale = 1.0;
    bool enabled = false;

    // unique_ptr so the listener user-data pointers stay valid as modes arrive.
    std::vector<std::unique_ptr<ModeItem>> modes;
    zwlr_output_mode_v1* current_mode_obj = nullptr;
};

} // namespace brodisplays
