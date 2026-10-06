# brodisplays

[![CI](https://github.com/wlejon/brodisplays/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/brodisplays/actions/workflows/ci.yml)
[![CodeQL](https://github.com/wlejon/brodisplays/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/brodisplays/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A high-performance, standalone C++20 cross-platform library for display enumeration, configuration, EDID parsing, scale/DPI calculation, HDR metadata, color profiles, panel backlight brightness, night light / gamma management, and test-then-revert configuration semantics.

Part of the **[bro ecosystem](https://github.com/wlejon/bro/blob/main/docs/ecosystem.md)**. [brocompositor](https://github.com/wlejon/brocompositor) consumes its display topology on Windows and macOS, and tests against it on Linux (`test_wl_brodisplays`).

---

## Features

- **Cross-Platform Display Service**: Unified abstract interface `DisplayService` with native implementations across Windows, Linux, and macOS.
- **Display Enumeration & Properties**:
  - Stable display identifier, user-friendly monitor name, adapter name, and manufacturer.
  - Native geometry bounds (x, y, width, height) and physical dimensions (mm).
  - Current mode and complete list of available modes (width, height, refresh rate).
  - Scale factor and DPI reporting (Retina, Per-Monitor DPI v2, Wayland/X11 scaling).
  - Display orientation (Normal, 90°, 180°, 270°).
  - Connection status and primary display identification.
  - `device_name`, the OS's own handle for the display (GDI `\\.\DISPLAYn` on Windows, `CGDirectDisplayID` on macOS, connector/output name on Linux), so hosts can join with native per-monitor APIs.
  - `is_active` (false while a display sleeps or is disabled) and `mirror_of` (the display a mirror shows).
- **Hardware EDID Parser**:
  - Full 128-byte base EDID checksum validation.
  - Compressed 5-bit PNP 3-letter manufacturer ID decoding (e.g. `SAM` -> Samsung).
  - Extraction of standard timings, established timings, and detailed timing descriptors.
  - Monitor descriptor parsing (display name, serial number).
- **Test-Then-Revert Semantics**:
  - `apply_temporary_configuration(change, timeout)`: Applies display configuration changes with an automatic rollback timer.
  - `confirm_configuration()`: Commits changes if the user accepts them, and persists them where the OS keeps a display store (Windows CCD database, macOS WindowServer preferences; on Linux they last for the session). A failed persist keeps the change pending and the timer running.
  - `revert_configuration()`: Instantly reverts back to previous stable parameters.
  - **Destructor Safety**: Reverts back automatically if the service is destroyed while a temporary configuration is pending.
- **Reactive Event Queue**:
  - Thread-safe `MessageQueue<DisplayEvent>` delivering value snapshots on display change, revert countdown ticks, revert completion, and confirmation.
- **Color & Night Light**:
  - Color profile retrieval (ICC / ColorSync / Windows Advanced Color).
  - Color temperature adjustment (Kelvin to RGB gamma ramp) for night light / night shift.
- **Panel Backlight (Linux)**:
  - `BacklightManager` (`brodisplays/backlight.h`) enumerates `/sys/class/backlight` devices, ties each to its DRM connector (e.g. `eDP-1`), reads and sets brightness raw or normalized (linear or perceptual gamma-2.2 curve), and watches for changes (inotify on `actual_brightness`, delivered as `BrightnessEvent` to a callback or either queue).
  - Brightness is set through logind's `SetBrightness` (no root needed; requires the GIO build) and falls back to writing sysfs directly. `BacklightManager::create()` returns nullptr on Windows and macOS.
- **Honest Capability Reporting**:
  - No synthetic or mock values in public APIs. Features unsupported by a platform or compositor return clean error results.

---

## Platform Backends

Stated honestly, what was verified where:

| Feature | Windows (`win/`) | Linux (`linux/`) | macOS (`mac/`) |
| :--- | :--- | :--- | :--- |
| **Primary Backend** | Windows CCD (`QueryDisplayConfig` / `SetDisplayConfig`) | Wayland (`zwlr_output_management_v1` + `wl_output`) | CoreGraphics (`CGGetOnlineDisplayList`, `CGConfigureDisplayWithDisplayMode`) |
| **Fallback Backend**| GDI (`EnumDisplaySettingsExW`, `ChangeDisplaySettingsExW`) | X11 XRandR 1.5+ (over XCB with timeout watchdog) / DRM Sysfs (`/sys/class/drm`) | IOKit (`IODisplayConnect`) |
| **Scale / DPI** | `GetDpiForMonitor` (Per-Monitor DPI v2) | Wayland scale factor / XRandR DPI | Points-to-pixels scale calculation |
| **EDID Source** | CCD `DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME` / WMI | XRandR `EDID` atom / Sysfs `/sys/class/drm/*/edid` | IOKit `IODisplayEDID` dictionary |
| **Night Light** | Windows GDI Gamma Ramp (`SetDeviceGammaRamp`) | KDE Plasma (KWin D-Bus) → GNOME (gsd-color GSettings + D-Bus) → `wlr-gamma-control` → RandR CRTC gamma; refused with reason otherwise | CoreGraphics Display Transfer Table (`CGSetDisplayTransferByTable`) |
| **Confirm Persists To** | CCD database (`SDC_SAVE_TO_DATABASE`) / GDI registry | Session only (no client-writable display store) | WindowServer preferences (`kCGConfigurePermanently`) |
| **Hotplug Events** | Hidden top-level HWND (`WM_DISPLAYCHANGE`; `WM_SETTINGCHANGE` / `WM_DPICHANGED` / `WM_DEVICECHANGE` when topology differs) | Wayland registry / RandR event loop over XCB with non-blocking pipe wake | `CGDisplayRegisterReconfigurationCallback` plus 500 ms topology poll |
| **Panel Backlight** | Unavailable (`nullptr`) | `/sys/class/backlight` sysfs + logind `SetBrightness` via D-Bus + inotify | Unavailable (`nullptr`) |

### Verified Configurations

- **Linux**: Ubuntu 24.04 LTS (x86_64), GCC 14, Clang 18, Wayland (`zwlr_output_management_v1`), X11 XCB RandR, DRM sysfs fallback, GIO/D-Bus night light.
- **Windows**: Windows Server 2022 (x64), MSVC 2022 (v143), CCD database and GDI fallbacks.
- **macOS**: macOS 15 Sequoia (arm64), Apple Clang 16, CoreGraphics display modes, IOKit EDID parsing, ColorSync.

---

## Directory Structure

```
include/brodisplays/
  brodisplays.h          # Umbrella header
  display_service.h      # Abstract DisplayService & DisplayServiceConfig
  backlight.h            # BacklightManager (Linux panel brightness & inotify)
  edid.h                 # Standalone EDID parser API
  event_queue.h          # Thread-safe MessageQueue & DisplayEvent variant
  types.h                # DisplayInfo, DisplayMode, Geometry, Scale, Results
src/
  common/
    edid.cpp             # EDID base checksum & descriptor decoding
    revert_timer.h/.cpp  # RevertManager background countdown timer & rollback
    color_temperature.h/.cpp # Kelvin to RGB gains (shared by all gamma paths)
    types.cpp            # Result formatting & helpers
  win/
    win_display_service.h/.cpp # CCD + GDI display service
    win_events.h/.cpp    # Hidden top-level watcher window + message pump
    win_gamma.h/.cpp     # Kelvin-to-gamma ramp calculator
  linux/
    drm_sysfs_backend.h/.cpp   # Headless /sys/class/drm parser
    linux_display_service.h/.cpp # Priority dispatcher (Wayland -> X11 -> DRM)
    wayland_backend.h/.cpp     # wlr-output-management & wl_output backend
    wayland_gamma.cpp          # wlr-gamma-control night light
    desktop_night_light.h/.cpp # KWin / GNOME night light over D-Bus (GIO)
    x11_connection.h/.cpp      # Bounded XCB connection + watchdog
    x11_backend.h/.cpp         # RandR outputs, CRTCs, modes, gamma over XCB
    backlight/                 # Linux backlight sysfs & logind integration
  mac/
    mac_display_service.h/.mm  # CoreGraphics + IOKit display service
    mac_events.h/.mm           # CGDisplayRegisterReconfigurationCallback
    mac_gamma.h/.mm            # CoreGraphics transfer table night shift
```

All source files are strictly maintained under 1,000 lines.

---

## Quick Start & API Usage

### 1. Enumerate Displays

```cpp
#include <brodisplays/brodisplays.h>
#include <iostream>

int main() {
    std::string err;
    auto service = brodisplays::DisplayService::create({}, &err);
    if (!service) {
        std::cerr << "Failed to initialize display service: " << err << "\n";
        return 1;
    }

    auto snapshot = service->snapshot();
    for (const auto& d : snapshot.displays) {
        std::cout << "Display: " << d.name << " (" << d.id << ")\n"
                  << "  Primary: " << (d.is_primary ? "Yes" : "No") << "\n"
                  << "  Current Mode: " << d.current_mode.width << "x" << d.current_mode.height
                  << " @ " << d.current_mode.refresh_rate << " Hz\n"
                  << "  Scale Factor: " << d.scale.factor << " (" << d.scale.dpi << " DPI)\n";
        if (!d.edid.raw_bytes.empty()) {
            std::cout << "  Manufacturer: " << d.edid.manufacturer_id << "\n";
        }
    }
    return 0;
}
```

### 2. Test-Then-Revert Mode Change

```cpp
// Change resolution to 1920x1080 with a 15-second automatic rollback timer
brodisplays::DisplayConfigChange change;
change.display_id = primary_id;
change.width = 1920;
change.height = 1080;
change.refresh_rate = 60.0;

auto res = service->apply_temporary_configuration(change, std::chrono::seconds(15));
if (res.ok) {
    std::cout << "Applied temporary mode. Confirm within 15 seconds...\n";
    // If user confirms:
    service->confirm_configuration();
    // If user reverts or cancels:
    // service->revert_configuration();
    // If neither is called, the timer restores original mode on expiry or destruction.
}
```

### 3. Night Light / Color Temperature

```cpp
// Set a warm color temperature (4500 Kelvin)
service->set_night_light(true, 4500);

// Restore original color table / desktop schedule
service->set_night_light(false, 6500);
```

### 4. Listen for Display Changes

```cpp
brodisplays::DisplayServiceConfig config;
config.enable_events = true;

auto service = brodisplays::DisplayService::create(config);

while (running) {
    if (!service->events().wait_for(std::chrono::milliseconds(500))) continue;
    for (const auto& event : service->events().drain()) {
        std::visit([](auto&& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, brodisplays::DisplaysChanged>) {
                std::cout << "Displays topology changed!\n";
            } else if constexpr (std::is_same_v<T, brodisplays::RevertCountdown>) {
                std::cout << "Reverting in " << ev.remaining.count() << " ms...\n";
            } else if constexpr (std::is_same_v<T, brodisplays::ConfigurationReverted>) {
                std::cout << "Configuration reverted"
                          << (ev.restored ? "" : " FAILED: " + ev.error) << "\n";
            }
        }, event);
    }
}
```

---

## Building

### Consuming `brodisplays::brodisplays`

Downstream projects link the `brodisplays::brodisplays` alias target. The library can be consumed either as a sibling checkout or as a Git submodule:

```cmake
# Resolving brodisplays: existing target -> sibling checkout -> submodule fallback
if(NOT TARGET brodisplays::brodisplays)
    set(BRODISPLAYS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../brodisplays" CACHE PATH "Path to brodisplays")
    if(EXISTS "${BRODISPLAYS_DIR}/CMakeLists.txt")
        add_subdirectory("${BRODISPLAYS_DIR}" "${CMAKE_BINARY_DIR}/brodisplays" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/brodisplays/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/brodisplays" "${CMAKE_BINARY_DIR}/brodisplays" EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "brodisplays not found at ${BRODISPLAYS_DIR} or third_party/brodisplays")
    endif()
endif()

target_link_libraries(my_app PRIVATE brodisplays::brodisplays)
```

### Sibling vs. Submodule Setup

#### Sibling Layout (Recommended for dev)

```bash
git clone https://github.com/wlejon/brocompositor
git clone https://github.com/wlejon/brodisplays   # placed next to consumers
```

#### Submodule Layout (Self-contained)

```bash
git clone --recursive https://github.com/wlejon/brodisplays
```

### Build Commands

#### Windows (MSVC 2022)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

#### Linux (GCC 12+ / Clang, Wayland + X11)

Dependencies: `libwayland-client`, `wayland-scanner`, `xcb`, `xcb-randr`, `libXau`; optional `gio-2.0` (`-DBRODISPLAYS_WITH_GIO`, on when found) for KDE / GNOME night light control.

```bash
# Debian / Ubuntu
sudo apt install cmake ninja-build pkg-config libwayland-dev wayland-scanner++ \
    libxcb1-dev libxcb-randr0-dev libxau-dev libglib2.0-dev
# Arch Linux
sudo pacman -S cmake ninja pkgconf wayland libxcb libxau glib2

cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure
```

#### macOS (Apple Clang 15+, CoreGraphics + IOKit)

```bash
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure
```

---

## Tests

### Safe Tests by Default

A plain `ctest` run never modifies the user's visible display configuration:

| Test | Coverage & Oracle |
| :--- | :--- |
| `test_edid` | EDID 128-byte checksum, PNP 3-letter vendor decoding, standard and detailed timing descriptor parsing |
| `test_event_queue` | MPSC `MessageQueue` push, drain, timeout waiting, and wake callback notification |
| `test_revert_timer` | Timer loop countdown ticks, cancel on confirm, rollback on expiry, and destructor auto-rollback |
| `test_backlight_curves` | Linear and perceptual gamma-2.2 curve conversions and normalization |
| `test_linux_query` | Querying outputs, modes, and geometry against the active backend (Wayland, X11, or DRM sysfs) |
| `test_linux_drm_sysfs` | Direct headless `/sys/class/drm` parsing (modes, EDID, connector status) |
| `test_linux_unresponsive` | Bounded connection timeout (2 s) preventing hangs when display servers stop responding |
| `test_linux_x11_stopped` | Watchdog recovery when an X11 server is paused (SIGSTOP) mid-session |
| `brodisplays_test_api` | Standalone Bronze JavaScript API bindings with GC stress testing |

### Tests That Change the Display (`BRODISPLAYS_TEST_MUTATE=1`)

Tests that switch display modes or load gamma ramps skip by default (exit code `77`). Set `BRODISPLAYS_TEST_MUTATE=1` to run them:

| Test | What It Mutates |
| :--- | :--- |
| `test_win_revert`, `test_mac_revert`, `test_linux_revert` | Switches the primary display to another refresh rate/resolution, verifies the rollback timer or explicit revert restores original mode, and checks `ConfigurationReverted` event |
| `test_win_gamma`, `test_mac_gamma` | Tints the primary display with a 4500 K night-light ramp, then verifies complete restoration |
| `test_win_confirm`, `test_mac_confirm` | Confirms a temporary refresh-rate change, verifies the OS store (CCD database / WindowServer preferences) recorded it, then restores the original mode |
| `test_linux_night_light_x11`, `_wlr`, `_kwin`, `_gnome` | Runs against private servers (Xorg dummy, headless sway, virtual KWin, headless mutter + gsd-color) with isolated runtime and D-Bus configs without affecting the host desktop |

```bash
BRODISPLAYS_TEST_MUTATE=1 ctest --test-dir build-release -R "revert|gamma" --output-on-failure
```

### What Skips on CI and Why

- **Mutating tests** (`test_*_revert`, `test_*_gamma`): Skip on standard runs unless `BRODISPLAYS_TEST_MUTATE=1` is explicitly set.
- **Private server tests** (`test_linux_night_light_*`): Skip when their respective private display servers (`sway`, `Xorg dummy`, `kwin`, `mutter`) or GSettings schemas are not installed on the system.
- **Backlight tests** (`test_linux_backlight`): Skip when running on machines without a physical display panel exposed under `/sys/class/backlight`.
- **macOS sleep**: macOS skips mode changes while the display sleeps (the runner uses `caffeinate -u` to wake it).

---

## License

[MIT](LICENSE)
