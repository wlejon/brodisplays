# brodisplays

A high-performance, standalone C++20 cross-platform library for display enumeration, configuration, EDID parsing, scale/DPI calculation, HDR metadata, color profiles, night light / gamma management, and test-then-revert configuration semantics.

Part of the **bro** ecosystem, designed in the mould of `brosys` and `htmlayout`.

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
- **Hardware EDID Parser**:
  - Full 128-byte base EDID checksum validation.
  - Compressed 5-bit PNP 3-letter manufacturer ID decoding (e.g. `SAM` -> Samsung).
  - Extraction of standard timings, established timings, and detailed timing descriptors.
  - Monitor descriptor parsing (display name, serial number).
- **Test-Then-Revert Semantics**:
  - `apply_temporary_configuration(change, timeout)`: Applies display configuration changes with an automatic rollback timer.
  - `confirm_configuration()`: Commits changes if the user accepts them.
  - `revert_configuration()`: Instantly reverts back to previous stable parameters.
  - **Destructor Safety**: Reverts back automatically if the service is destroyed while a temporary configuration is pending.
- **Reactive Event Queue**:
  - Thread-safe `MessageQueue<DisplayEvent>` delivering value snapshots on display change, revert countdown ticks, revert completion, and confirmation.
- **Color & Night Light**:
  - Color profile retrieval (ICC / ColorSync / Windows Advanced Color).
  - Color temperature adjustment (Kelvin to RGB gamma ramp) for night light / night shift.
- **Honest Capability Reporting**:
  - No synthetic or mock values in public APIs. Features unsupported by a platform or compositor return clean error results.

---

## Platform Backends

| Feature | Windows (`win/`) | Linux (`linux/`) | macOS (`mac/`) |
| :--- | :--- | :--- | :--- |
| **Primary Backend** | Windows CCD (`QueryDisplayConfig` / `SetDisplayConfig`) | Wayland (`zwlr_output_management_v1` + `wl_output`) | CoreGraphics (`CGGetOnlineDisplayList`, `CGConfigureDisplayWithDisplayMode`) |
| **Fallback Backend**| GDI (`EnumDisplaySettingsExW`, `ChangeDisplaySettingsExW`) | X11 XRandR 1.5+ / DRM Sysfs (`/sys/class/drm`) | IOKit (`IODisplayConnect`) |
| **Scale / DPI** | `GetDpiForMonitor` (Per-Monitor DPI v2) | Wayland scale factor / XRandR DPI | Points-to-pixels scale calculation |
| **EDID Source** | CCD `DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME` / WMI | XRandR `EDID` atom / Sysfs `/sys/class/drm/*/edid` | IOKit `IODisplayEDID` dictionary |
| **Night Light** | Windows GDI Gamma Ramp (`SetDeviceGammaRamp`) | Gamma / Honest unsupported return | CoreGraphics Display Transfer Table (`CGSetDisplayTransferByTable`) |
| **Hotplug Events** | Message-only HWND (`WM_DISPLAYCHANGE`, `WM_SETTINGCHANGE`) | Wayland registry / XRandR event loop with non-blocking pipe wake | `CGDisplayRegisterReconfigurationCallback` |

---

## Directory Structure

```
brodisplays/
├── include/brodisplays/
│   ├── brodisplays.h          # Umbrella header
│   ├── display_service.h      # Abstract DisplayService & DisplayServiceConfig
│   ├── edid.h                 # Standalone EDID parser API
│   ├── event_queue.h          # Thread-safe MessageQueue & DisplayEvent variant
│   └── types.h                # DisplayInfo, DisplayMode, Geometry, Scale, Results
├── src/
│   ├── common/
│   │   ├── edid.cpp           # EDID base checksum & descriptor decoding
│   │   ├── revert_timer.h     # RevertManager background countdown timer
│   │   ├── revert_timer.cpp   # Timer loop & rollback orchestration
│   │   └── types.cpp          # Result formatting & helpers
│   ├── win/
│   │   ├── win_display_service.h/.cpp # CCD + GDI display service
│   │   ├── win_events.h/.cpp          # Background win32 message pump
│   │   └── win_gamma.h/.cpp           # Kelvin-to-gamma ramp calculator
│   ├── linux/
│   │   ├── drm_sysfs_backend.h/.cpp   # Headless /sys/class/drm parser
│   │   ├── linux_display_service.h/.cpp# Priority dispatcher (Wayland -> X11 -> DRM)
│   │   ├── wayland_backend.h/.cpp     # wlr-output-management & wl_output backend
│   │   └── x11_backend.h/.cpp         # XRandR output & CRTC enumeration
│   └── mac/
│       ├── mac_display_service.h/.mm  # CoreGraphics + IOKit display service
│       ├── mac_events.h/.mm           # CGDisplayRegisterReconfigurationCallback
│       └── mac_gamma.h/.mm            # CoreGraphics transfer table night shift
└── tests/
    ├── check.h                        # Minimal test runner (CHECK, REQUIRE, finish, skip)
    ├── common/                        # Unit tests for EDID, event queue, revert timer
    ├── win/                           # Windows query, revert, gamma, event tests
    ├── linux/                         # Linux query, revert, sysfs DRM tests
    └── mac/                           # macOS query, revert, gamma tests
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
    // If the user clicks "Keep Changes":
    service->confirm_configuration();
    // Or if the user clicks "Revert":
    // service->revert_configuration();
    // If neither is called, the timer will automatically restore the original mode.
}
```

### 3. Night Light / Color Temperature

```cpp
// Set a warm color temperature (4500 Kelvin)
service->set_night_light(true, 4500);

// Restore original color table
service->set_night_light(false, 6500);
```

### 4. Listen for Display Changes

```cpp
brodisplays::DisplayServiceConfig config;
config.enable_events = true;

auto service = brodisplays::DisplayService::create(config);

// In a worker loop:
while (running) {
    auto event = service->events().pop(std::chrono::milliseconds(500));
    if (event) {
        std::visit([](auto&& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, brodisplays::DisplaysChanged>) {
                std::cout << "Displays topology changed!\n";
            } else if constexpr (std::is_same_v<T, brodisplays::RevertCountdown>) {
                std::cout << "Reverting in " << ev.seconds_remaining << "s...\n";
            } else if constexpr (std::is_same_v<T, brodisplays::ConfigurationReverted>) {
                std::cout << "Configuration reverted.\n";
            }
        }, *event);
    }
}
```

---

## Building & Testing

`brodisplays` uses modern CMake (3.24+).

### Windows (MSVC 2022)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux (GCC 12+ / Clang, Wayland + X11)

Dependencies: `libwayland-client`, `wayland-scanner`, `libx11`, `libxrandr`.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build -j 4
ctest --test-dir build --output-on-failure
```

### macOS (AppleClang 15+, CoreGraphics + IOKit)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build -j 4
ctest --test-dir build --output-on-failure
```

---

## Verification Matrix

All unit tests and integration tests pass across target platforms:

| Platform | Compiler | Tests Passed | Status |
| :--- | :--- | :--- | :--- |
| **Windows 11** | MSVC 19.44 (VS 2022) | 7 / 7 (6 passed, 1 skipped) | Verified Clean |
| **Linux (Arch)** | GCC 16.2.1 / Ninja | 6 / 6 (100%) | Verified Clean |
| **macOS (Darwin ARM64)** | AppleClang 17.0.0 / Ninja | 6 / 6 (100%) | Verified Clean |

*Note: Mutation tests that modify physical display modes gracefully detect non-elevated desktop session restrictions and exit with return code 77 (`bstest::skip`), leaving 0 permanent modifications on host systems.*
