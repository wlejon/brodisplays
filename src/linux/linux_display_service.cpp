#include "linux_display_service.h"

#include <iostream>

namespace brodisplays {

std::unique_ptr<DisplayService> DisplayService::create(
    const DisplayServiceConfig& config,
    std::string* error) {
    try {
        return std::make_unique<LinuxDisplayService>(config);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

LinuxDisplayService::LinuxDisplayService(const DisplayServiceConfig& config)
    : config_(config) {
    revert_manager_ = std::make_unique<RevertManager>(events_);

    // One display-server backend serves a session: enumeration, mode changes
    // and hotplug events must all speak about the same outputs. Under a
    // Wayland session XRandR only sees Xwayland's emulated outputs (different
    // names, and mode sets there never reach the real displays), so X11 is
    // used only when there is no Wayland compositor to talk to.
    std::string err;
    wayland_backend_ = WaylandBackend::create(&err);
    if (!wayland_backend_) {
        x11_backend_ = X11Backend::create(&err);
    }
    if (config_.enable_gamma) {
        desktop_night_light_ = DesktopNightLight::detect();
    }

    last_snapshot_ = snapshot();

    if (config_.enable_events) {
        if (wayland_backend_) {
            wayland_backend_->start_watcher([this] { on_system_display_change(); });
        } else if (x11_backend_) {
            x11_backend_->start_watcher([this] { on_system_display_change(); });
        }
    }
}

LinuxDisplayService::~LinuxDisplayService() {
    if (x11_backend_) x11_backend_->stop_watcher();
    if (wayland_backend_) wayland_backend_->stop_watcher();
    revert_manager_.reset();
    // Gamma ramps a backend set are handed back as it goes (X11 restores
    // them; a Wayland compositor does once the connection closes). A
    // desktop's night light is the desktop's setting and stays as set.
}

MessageQueue<DisplayEvent>& LinuxDisplayService::events() {
    return events_;
}

void LinuxDisplayService::on_system_display_change() {
    auto new_snap = snapshot();
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        last_snapshot_ = new_snap;
    }
    events_.push(DisplaysChanged{new_snap});
}

void LinuxDisplayService::enrich_with_sysfs_edid(std::vector<DisplayInfo>& displays) const {
    auto sysfs_displays = DrmSysfsBackend::enumerate();
    for (auto& d : displays) {
        for (const auto& s : sysfs_displays) {
            if (d.id == s.id || d.name == s.id || s.id.find(d.id) != std::string::npos || d.id.find(s.id) != std::string::npos) {
                if (!s.edid.raw_bytes.empty()) {
                    d.edid = s.edid;
                    if (!s.edid.monitor_name.empty()) {
                        d.name = s.edid.monitor_name;
                        d.model = s.edid.monitor_name;
                    }
                    if (!s.edid.manufacturer_id.empty()) {
                        d.manufacturer = s.edid.manufacturer_id;
                    }
                    if (s.physical_width_mm > 0 && d.physical_width_mm == 0) {
                        d.physical_width_mm = s.physical_width_mm;
                        d.physical_height_mm = s.physical_height_mm;
                    }
                }
                break;
            }
        }
    }
}

DisplaysSnapshot LinuxDisplayService::snapshot() const {
    DisplaysSnapshot snap;
    snap.timestamp = std::chrono::system_clock::now();

    // The session's display server first, then the kernel's view (DRM sysfs)
    // when there is none or it has stopped answering.
    if (wayland_backend_) {
        snap.displays = wayland_backend_->snapshot();
    } else if (x11_backend_) {
        snap.displays = x11_backend_->snapshot();
    }
    if (snap.displays.empty()) {
        snap.displays = DrmSysfsBackend::enumerate();
    }

    // Enrich with hardware EDID if missing
    enrich_with_sysfs_edid(snap.displays);

    // A desktop's night light covers every display; where a desktop owns the
    // gamma ramps but cannot be driven (no GIO), nothing here can.
    if (desktop_night_light_) {
        NightLightStatus st = desktop_night_light_->status();
        for (auto& d : snap.displays) d.night_light = st;
    } else if (desktop_owns_gamma() && !wayland_backend_) {
        for (auto& d : snap.displays) d.night_light = NightLightStatus{};
    }

    return snap;
}

Result LinuxDisplayService::apply_configuration(const DisplayConfigChange& change) {
    if (wayland_backend_) {
        return wayland_backend_->apply_configuration(change);
    }
    if (x11_backend_) {
        return x11_backend_->apply_configuration(change);
    }
    return Result::failure("No configurable display backend (X11 or Wayland wlr-output-management) available");
}

Result LinuxDisplayService::apply_temporary_configuration(
    const DisplayConfigChange& change,
    std::chrono::milliseconds timeout) {
    auto snap = snapshot();
    const auto* d = change.display_id.empty() ? snap.primary_display() : snap.find_display(change.display_id);
    if (!d) {
        return Result::failure("Display not found: " + change.display_id);
    }

    DisplayConfigChange orig_cfg;
    orig_cfg.display_id = d->id;
    orig_cfg.width = d->current_mode.width;
    orig_cfg.height = d->current_mode.height;
    orig_cfg.refresh_rate = d->current_mode.refresh_rate;
    orig_cfg.x = d->geometry.x;
    orig_cfg.y = d->geometry.y;
    orig_cfg.orientation = d->orientation;
    orig_cfg.scale_factor = d->scale.factor;

    Result res = apply_configuration(change);
    if (!res) {
        return res;
    }

    revert_manager_->start_temporary(
        d->id, timeout,
        [this, orig_cfg]() {
            return apply_configuration(orig_cfg);
        });

    return Result::success();
}

Result LinuxDisplayService::confirm_configuration() {
    return revert_manager_->confirm();
}

Result LinuxDisplayService::revert_configuration() {
    return revert_manager_->revert();
}

bool LinuxDisplayService::is_revert_pending() const {
    return revert_manager_->is_pending();
}

// The desktop's own night light first (it owns the ramps and would undo or
// fight anything else), then gamma ramps where nothing else owns them:
// wlr-gamma-control on wlroots compositors, RandR CRTC gamma on X11.
Result LinuxDisplayService::set_night_light(bool enabled, uint32_t temperature_kelvin) {
    std::lock_guard<std::mutex> lock(night_light_mutex_);
    if (desktop_night_light_) {
        return desktop_night_light_->set(enabled, temperature_kelvin);
    }
    if (wayland_backend_) {
        if (wayland_backend_->gamma_supported()) return wayland_backend_->set_night_light(enabled, temperature_kelvin);
        return Result::failure(desktop_owns_gamma()
                                   ? "The desktop's night light cannot be driven (brodisplays was built without GIO)"
                                   : "The Wayland compositor offers no night light control: no output takes a "
                                     "wlr-gamma-control ramp (protocol missing, outputs without gamma, or held "
                                     "by another client) and no KDE or GNOME night light service runs");
    }
    if (x11_backend_) {
        if (desktop_owns_gamma()) {
            return Result::failure("The desktop's night light cannot be driven (brodisplays was built without GIO)");
        }
        if (x11_backend_->broken()) return Result::failure("The X server stopped answering");
        return x11_backend_->set_night_light(enabled, temperature_kelvin);
    }
    return Result::failure("Night light needs a display server (no Wayland compositor or X server in this session)");
}

} // namespace brodisplays
