#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "brodisplays/display_service.h"
#include "brodisplays/event_queue.h"
#include "common/revert_timer.h"
#include "mac_events.h"
#include "mac_gamma.h"

namespace brodisplays {

class MacDisplayService : public DisplayService {
public:
    explicit MacDisplayService(const DisplayServiceConfig& config);
    ~MacDisplayService() override;

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

    DisplayServiceConfig config_;
    MessageQueue<DisplayEvent> events_;
    std::unique_ptr<RevertManager> revert_manager_;
    std::unique_ptr<MacDisplayWatcher> watcher_;
    MacGamma gamma_;

    mutable std::mutex snapshot_mutex_;
    mutable DisplaysSnapshot last_snapshot_;
};

} // namespace brodisplays
