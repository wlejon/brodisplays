#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "brodisplays/event_queue.h"
#include "brodisplays/types.h"

namespace brodisplays {

struct DisplayServiceConfig {
    bool enable_events = true;
    bool enable_gamma = true;
};

class DisplayService {
public:
    static std::unique_ptr<DisplayService> create(
        const DisplayServiceConfig& config = DisplayServiceConfig(),
        std::string* error = nullptr);

    virtual ~DisplayService() = default;

    // Returns a fresh value-snapshot of all currently connected displays.
    virtual DisplaysSnapshot snapshot() const = 0;

    // Event queue carrying snapshot changes and revert notifications to the host.
    virtual MessageQueue<DisplayEvent>& events() = 0;

    // Immediately and permanently applies a display configuration change.
    virtual Result apply_configuration(const DisplayConfigChange& change) = 0;

    // Test-then-revert semantics (like OS "Keep these changes?"):
    // Applies the configuration change with an automatic revert timer.
    // If confirm_configuration() is not called before timeout expires,
    // the configuration automatically rolls back to the previous snapshot.
    virtual Result apply_temporary_configuration(
        const DisplayConfigChange& change,
        std::chrono::milliseconds timeout = std::chrono::seconds(10)) = 0;

    // Confirms the pending temporary configuration and stops the revert
    // timer. Where the OS keeps a display-configuration store it is
    // persisted there, so it survives logout and reboot: the CCD database on
    // Windows (SetDisplayConfig SDC_SAVE_TO_DATABASE, or the GDI registry on
    // the fallback path), WindowServer's display preferences on macOS
    // (kCGConfigurePermanently). Linux has no such store a client can write
    // (the compositor or X server owns its own configuration files), so
    // there it lasts for the session. If persisting fails the configuration
    // stays pending, the error is returned, and the revert timer runs on.
    virtual Result confirm_configuration() = 0;

    // Manually rolls back any pending temporary configuration immediately.
    virtual Result revert_configuration() = 0;

    // Checks whether an unconfirmed temporary configuration is currently active.
    virtual bool is_revert_pending() const = 0;

    // Controls Night Light / Night Shift / color temperature. On Linux the
    // desktop's own night light is driven where one runs (KDE Plasma's KWin,
    // GNOME's settings daemon), else gamma ramps where nothing else owns
    // them (wlr-gamma-control on wlroots compositors, RandR CRTC gamma on
    // X11); anything else fails with the reason.
    virtual Result set_night_light(bool enabled, uint32_t temperature_kelvin = 4500) = 0;
};

} // namespace brodisplays
