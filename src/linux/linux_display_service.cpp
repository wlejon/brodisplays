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

    std::string err;
    if (WaylandBackend::is_available()) {
        wayland_backend_ = WaylandBackend::create(&err);
    }
    if (X11Backend::is_available()) {
        x11_backend_ = X11Backend::create(&err);
    }

    last_snapshot_ = snapshot();

    if (config_.enable_events) {
        if (x11_backend_) {
            x11_backend_->start_watcher([this] { on_system_display_change(); });
        } else if (wayland_backend_) {
            wayland_backend_->start_watcher([this] { on_system_display_change(); });
        }
    }
}

LinuxDisplayService::~LinuxDisplayService() {
    if (x11_backend_) x11_backend_->stop_watcher();
    if (wayland_backend_) wayland_backend_->stop_watcher();
    revert_manager_.reset();
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

    // Prefer Wayland, then X11, then DRM sysfs
    if (wayland_backend_) {
        snap.displays = wayland_backend_->snapshot();
    }
    if (snap.displays.empty() && x11_backend_) {
        snap.displays = x11_backend_->snapshot();
    }
    if (snap.displays.empty()) {
        snap.displays = DrmSysfsBackend::enumerate();
    }

    // Enrich with hardware EDID if missing
    enrich_with_sysfs_edid(snap.displays);

    return snap;
}

Result LinuxDisplayService::apply_configuration(const DisplayConfigChange& change) {
    if (x11_backend_) {
        return x11_backend_->apply_configuration(change);
    }
    if (wayland_backend_) {
        return wayland_backend_->apply_configuration(change);
    }
    return Result::failure("No configurable display backend (X11 or Wayland wlr-output-management) available");
}

Result LinuxDisplayService::apply_temporary_configuration(
    const DisplayConfigChange& change,
    std::chrono::milliseconds timeout) {
    auto snap = snapshot();
    const auto* d = snap.find_display(change.display_id);
    if (!d && !snap.displays.empty()) {
        d = &snap.displays.front();
    }
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
            apply_configuration(orig_cfg);
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

Result LinuxDisplayService::set_night_light(bool /*enabled*/, uint32_t /*temperature_kelvin*/) {
    // Honest capability reporting: not supported without color manager or gamma protocol
    return Result::failure("Night light not supported on this Linux display server");
}

} // namespace brodisplays
