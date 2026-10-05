#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "brodisplays/display_service.h"
#include "brodisplays/event_queue.h"
#include "common/revert_timer.h"
#include "win_events.h"
#include "win_gamma.h"

namespace brodisplays {

class WinDisplayService : public DisplayService {
public:
    explicit WinDisplayService(const DisplayServiceConfig& config);
    ~WinDisplayService() override;

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
    DisplayInfo build_display_info(
        const DISPLAYCONFIG_PATH_INFO& path,
        const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
        size_t index) const;

    DisplayServiceConfig config_;
    MessageQueue<DisplayEvent> events_;
    std::unique_ptr<RevertManager> revert_manager_;
    std::unique_ptr<WinDisplayWatcher> watcher_;
    WinGamma gamma_;

    mutable std::mutex snapshot_mutex_;
    mutable DisplaysSnapshot last_snapshot_;
    // Display id (monitor device path) -> GDI source name ("\\.\DISPLAYn").
    // Written by snapshot() on the caller's and the watcher's threads.
    mutable std::mutex gdi_mutex_;
    mutable std::unordered_map<std::string, std::wstring> gdi_name_map_;
    std::wstring gdi_name_for(const std::string& display_id) const;
};

} // namespace brodisplays
