#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "brodisplays/display_service.h"
#include "brodisplays/event_queue.h"
#include "common/revert_timer.h"
#include "drm_sysfs_backend.h"
#include "wayland_backend.h"
#include "x11_backend.h"

namespace brodisplays {

class LinuxDisplayService : public DisplayService {
public:
    explicit LinuxDisplayService(const DisplayServiceConfig& config);
    ~LinuxDisplayService() override;

    DisplaysSnapshot snapshot() const override;
    MessageQueue<DisplayEvent>& events() override;

    Result apply_configuration(const DisplayConfigChange& change) override;
    Result apply_temporary_configuration(
        const DisplayConfigChange& change,
        std::chrono::milliseconds timeout = std::chrono::seconds(10)) override;
    Result confirm_configuration() override;
    Result revert_configuration() override;
    bool is_revert_pending() const override;

    Result set_night_light(bool enabled, uint32_t temperature_kelvin = 4500) override;

private:
    void on_system_display_change();
    void enrich_with_sysfs_edid(std::vector<DisplayInfo>& displays) const;

    DisplayServiceConfig config_;
    MessageQueue<DisplayEvent> events_;
    std::unique_ptr<RevertManager> revert_manager_;

    std::unique_ptr<WaylandBackend> wayland_backend_;
    std::unique_ptr<X11Backend> x11_backend_;

    mutable std::mutex snapshot_mutex_;
    mutable DisplaysSnapshot last_snapshot_;
};

} // namespace brodisplays
