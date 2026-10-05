#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "brodisplays/types.h"

namespace brodisplays {

// A desktop environment's own night light, driven through its own settings
// so it stays in charge (and the change behaves exactly like the user
// flipping the switch in System Settings, persisting the same way):
//
//  * KDE Plasma: KWin's night light. Settings in kwinrc [NightColor]
//    (Active, Mode=Constant, NightTemperature), announced with the
//    org.kde.kconfig.notify ConfigChanged signal KWin listens for; state read
//    back from org.kde.KWin.NightLight on the session bus.
//  * GNOME: the org.gnome.settings-daemon.plugins.color GSettings (enabled,
//    temperature, a schedule covering the whole day); state read back from
//    org.gnome.SettingsDaemon.Color, support from mutter's
//    org.gnome.Mutter.DisplayConfig NightLightSupported.
//
// Every D-Bus exchange is bounded (kTimeout). Needs GIO (BRODISPLAYS_WITH_GIO);
// without it detect() finds nothing.
class DesktopNightLight {
public:
    static constexpr int kTimeoutMs = 2000;

    // The desktop that owns night light in this session, or nullptr.
    static std::unique_ptr<DesktopNightLight> detect();

    virtual ~DesktopNightLight() = default;
    virtual const char* name() const = 0;
    virtual NightLightStatus status() = 0;
    // Succeeds only once the desktop reports the requested state.
    virtual Result set(bool enabled, uint32_t temperature_kelvin) = 0;
};

// Whether $XDG_CURRENT_DESKTOP names a desktop whose night light (or colour
// management) owns the gamma ramps, so a raw gamma fallback would fight it.
bool desktop_owns_gamma();

} // namespace brodisplays
