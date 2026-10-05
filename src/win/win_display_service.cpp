#include "win_display_service.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellscalingapi.h>

#include "brodisplays/edid.h"

namespace brodisplays {

namespace {

std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || *wstr == 0) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &s[0], len, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &w[0], len);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

DisplayOrientation rotation_to_orientation(DISPLAYCONFIG_ROTATION rotation) {
    switch (rotation) {
        case DISPLAYCONFIG_ROTATION_IDENTITY: return DisplayOrientation::Normal;
        case DISPLAYCONFIG_ROTATION_ROTATE90: return DisplayOrientation::Rotate90;
        case DISPLAYCONFIG_ROTATION_ROTATE180: return DisplayOrientation::Rotate180;
        case DISPLAYCONFIG_ROTATION_ROTATE270: return DisplayOrientation::Rotate270;
        default: return DisplayOrientation::Normal;
    }
}

DisplayScale get_monitor_dpi(const std::wstring& gdi_name) {
    DisplayScale scale;
    scale.factor = 1.0;
    scale.dpi = 96;

    // Load Shcore.dll dynamically for GetDpiForMonitor
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT(WINAPI * PFN_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
        auto pfnGetDpiForMonitor = reinterpret_cast<PFN_GetDpiForMonitor>(
            GetProcAddress(shcore, "GetDpiForMonitor"));

        if (pfnGetDpiForMonitor) {
            POINT pt{0, 0};
            // Find HMONITOR from GDI name
            struct MonitorFinder {
                std::wstring target_name;
                HMONITOR found = nullptr;
            } finder{gdi_name, nullptr};

            EnumDisplayMonitors(
                nullptr, nullptr,
                [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
                    auto* f = reinterpret_cast<MonitorFinder*>(lp);
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(hMon, &mi)) {
                        if (f->target_name == mi.szDevice) {
                            f->found = hMon;
                            return FALSE;
                        }
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&finder));

            if (finder.found) {
                UINT dpiX = 96, dpiY = 96;
                if (SUCCEEDED(pfnGetDpiForMonitor(finder.found, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                    scale.dpi = static_cast<int32_t>(dpiX);
                    scale.factor = static_cast<double>(dpiX) / 96.0;
                }
            }
        }
        FreeLibrary(shcore);
    }
    return scale;
}

std::vector<DisplayMode> query_available_modes(const std::wstring& gdi_name) {
    std::vector<DisplayMode> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);

    for (DWORD i = 0; EnumDisplaySettingsExW(gdi_name.c_str(), i, &dm, 0); ++i) {
        if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0) continue;
        DisplayMode m{
            static_cast<uint32_t>(dm.dmPelsWidth),
            static_cast<uint32_t>(dm.dmPelsHeight),
            static_cast<double>(dm.dmDisplayFrequency)};

        bool exists = false;
        for (const auto& existing : modes) {
            if (existing == m) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            modes.push_back(m);
        }
    }
    return modes;
}

// Saves the active CCD configuration to the display database (what Windows
// restores at the next logon or reconnect of this monitor set). Re-applying
// the active configuration changes nothing on screen.
Result persist_active_configuration() {
    UINT32 path_count = 0, mode_count = 0;
    LONG err = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (err != ERROR_SUCCESS) {
        return Result::failure("GetDisplayConfigBufferSizes failed (error " + std::to_string(err) + ")");
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    err = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    if (err != ERROR_SUCCESS) {
        return Result::failure("QueryDisplayConfig failed (error " + std::to_string(err) + ")");
    }
    err = SetDisplayConfig(path_count, paths.data(), mode_count, modes.data(),
                           SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES);
    if (err != ERROR_SUCCESS) {
        return Result::failure("SetDisplayConfig(SDC_SAVE_TO_DATABASE) failed (error " + std::to_string(err) + ")");
    }
    return Result::success();
}

} // namespace

std::unique_ptr<DisplayService> DisplayService::create(
    const DisplayServiceConfig& config,
    std::string* error) {
    try {
        return std::make_unique<WinDisplayService>(config);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

WinDisplayService::WinDisplayService(const DisplayServiceConfig& config)
    : config_(config) {
    revert_manager_ = std::make_unique<RevertManager>(events_);

    // The baseline the watcher compares against exists before it starts.
    last_snapshot_ = snapshot();

    if (config_.enable_events) {
        watcher_ = std::make_unique<WinDisplayWatcher>([this](bool definite) {
            on_system_display_change(definite);
        });
    }
}

WinDisplayService::~WinDisplayService() {
    watcher_.reset();
    revert_manager_.reset();
    gamma_.restore_all();
}

MessageQueue<DisplayEvent>& WinDisplayService::events() {
    return events_;
}

// WM_DISPLAYCHANGE is always published; the broadcast hints (settings, DPI,
// device arrival) only when the displays actually differ from what was last
// published, so a theme or environment change does not look like a display
// change.
void WinDisplayService::on_system_display_change(bool definite) {
    auto new_snap = snapshot();
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (!definite && new_snap.same_displays(last_snapshot_)) return;
        last_snapshot_ = new_snap;
    }
    events_.push(DisplaysChanged{new_snap});
}

DisplaysSnapshot WinDisplayService::snapshot() const {
    DpiScope dpi;
    DisplaysSnapshot snap;
    snap.timestamp = std::chrono::system_clock::now();

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);

    if (status == ERROR_SUCCESS && pathCount > 0 && modeCount > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

        status = QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
            &modeCount, modes.data(), nullptr);

        if (status == ERROR_SUCCESS) {
            // Clone mode: several paths (targets) share one source. The first
            // target owns the desktop area; the others mirror it.
            struct Source {
                LUID adapter;
                UINT32 id;
                std::string display_id;
            };
            std::vector<Source> sources;
            for (size_t i = 0; i < pathCount; ++i) {
                DisplayInfo info = build_display_info(paths[i], modes, i);
                const auto& src = paths[i].sourceInfo;
                for (const auto& s : sources) {
                    if (s.adapter.LowPart == src.adapterId.LowPart &&
                        s.adapter.HighPart == src.adapterId.HighPart && s.id == src.id) {
                        info.mirror_of = s.display_id;
                        info.is_primary = false;
                        break;
                    }
                }
                if (info.mirror_of.empty()) sources.push_back(Source{src.adapterId, src.id, info.id});
                snap.displays.push_back(std::move(info));
            }
        }
    }

    // Fallback if CCD returned nothing (e.g. headless/RDP)
    if (snap.displays.empty()) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

            DisplayInfo info;
            info.id = wide_to_utf8(dd.DeviceName);
            info.device_name = info.id;
            info.name = wide_to_utf8(dd.DeviceString);
            info.adapter_name = wide_to_utf8(dd.DeviceString);
            info.is_primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            info.is_connected = true;

            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
                info.geometry.x = dm.dmPosition.x;
                info.geometry.y = dm.dmPosition.y;
                info.geometry.width = dm.dmPelsWidth;
                info.geometry.height = dm.dmPelsHeight;
                info.current_mode = DisplayMode{
                    static_cast<uint32_t>(dm.dmPelsWidth),
                    static_cast<uint32_t>(dm.dmPelsHeight),
                    static_cast<double>(dm.dmDisplayFrequency)};
            }

            info.scale = get_monitor_dpi(dd.DeviceName);
            info.available_modes = query_available_modes(dd.DeviceName);
            info.night_light = gamma_.get_status(info.id);
            snap.displays.push_back(info);
        }
    }

    return snap;
}

DisplayInfo WinDisplayService::build_display_info(
    const DISPLAYCONFIG_PATH_INFO& path,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    size_t index) const {
    DisplayInfo info;

    // 1. Target Name & Details
    DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
    targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    targetName.header.size = sizeof(targetName);
    targetName.header.adapterId = path.targetInfo.adapterId;
    targetName.header.id = path.targetInfo.id;

    if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS) {
        if (targetName.monitorFriendlyDeviceName[0] != 0) {
            info.name = wide_to_utf8(targetName.monitorFriendlyDeviceName);
            info.model = info.name;
        }
        info.id = wide_to_utf8(targetName.monitorDevicePath);
        if (info.id.empty()) {
            info.id = "\\\\.\\DISPLAY" + std::to_string(index + 1);
        }

        info.is_internal = (targetName.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                            targetName.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED);

        if (targetName.edidManufactureId != 0) {
            info.edid.manufacturer_id = decode_edid_manufacturer_id(targetName.edidManufactureId);
            info.edid.product_code = targetName.edidProductCodeId;
            info.manufacturer = info.edid.manufacturer_id;
        }
    } else {
        info.id = "\\\\.\\DISPLAY" + std::to_string(index + 1);
        info.name = "Generic Display " + std::to_string(index + 1);
    }

    // 2. Adapter Name
    DISPLAYCONFIG_ADAPTER_NAME adapterName{};
    adapterName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    adapterName.header.size = sizeof(adapterName);
    adapterName.header.adapterId = path.targetInfo.adapterId;
    if (DisplayConfigGetDeviceInfo(&adapterName.header) == ERROR_SUCCESS) {
        info.adapter_name = wide_to_utf8(adapterName.adapterDevicePath);
    }

    // 3. Source Mode (Geometry)
    if (path.sourceInfo.modeInfoIdx < modes.size()) {
        const auto& srcMode = modes[path.sourceInfo.modeInfoIdx];
        if (srcMode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            info.geometry.x = srcMode.sourceMode.position.x;
            info.geometry.y = srcMode.sourceMode.position.y;
            info.geometry.width = srcMode.sourceMode.width;
            info.geometry.height = srcMode.sourceMode.height;
            info.is_primary = (srcMode.sourceMode.position.x == 0 && srcMode.sourceMode.position.y == 0);
        }
    }

    // 4. Target Mode (Refresh rate)
    double refresh_rate = 60.0;
    if (path.targetInfo.modeInfoIdx < modes.size()) {
        const auto& tgtMode = modes[path.targetInfo.modeInfoIdx];
        if (tgtMode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            const auto& vsync = tgtMode.targetMode.targetVideoSignalInfo.vSyncFreq;
            if (vsync.Denominator > 0) {
                refresh_rate = static_cast<double>(vsync.Numerator) / static_cast<double>(vsync.Denominator);
            }
        }
    }

    info.current_mode = DisplayMode{
        info.geometry.width,
        info.geometry.height,
        std::round(refresh_rate * 100.0) / 100.0};

    // 5. Orientation
    info.orientation = rotation_to_orientation(path.targetInfo.rotation);

    // 6. HDR Capabilities
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colorInfo{};
    colorInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    colorInfo.header.size = sizeof(colorInfo);
    colorInfo.header.adapterId = path.targetInfo.adapterId;
    colorInfo.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&colorInfo.header) == ERROR_SUCCESS) {
        info.hdr.supported = colorInfo.advancedColorSupported;
        info.hdr.enabled = colorInfo.advancedColorEnabled;
    }

    // 7. Map to GDI name for DPI and Mode enumeration
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(sourceName);
    sourceName.header.adapterId = path.sourceInfo.adapterId;
    sourceName.header.id = path.sourceInfo.id;

    std::wstring gdiDeviceName;
    if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
        gdiDeviceName = sourceName.viewGdiDeviceName;
    }

    if (!gdiDeviceName.empty()) {
        info.device_name = wide_to_utf8(gdiDeviceName.c_str());
        info.scale = get_monitor_dpi(gdiDeviceName);
        info.available_modes = query_available_modes(gdiDeviceName);
        info.night_light = gamma_.get_status(wide_to_utf8(gdiDeviceName.c_str()));
    } else {
        info.scale.factor = 1.0;
        info.scale.dpi = 96;
    }

    return info;
}

Result WinDisplayService::apply_configuration(const DisplayConfigChange& change) {
    DpiScope dpi;
    std::wstring gdi_name;

    // Resolve GDI name from display_id (an empty id means the primary display)
    auto snap = snapshot();
    const auto* d = change.display_id.empty() ? snap.primary_display() : snap.find_display(change.display_id);
    if (!d) {
        return Result::failure("Display not found: " + change.display_id);
    }
    gdi_name = utf8_to_wide(d->device_name);
    if (gdi_name.empty()) {
        return Result::failure("No GDI device drives display " + d->id);
    }

    DEVMODEW cur_dm{};
    cur_dm.dmSize = sizeof(cur_dm);

    if (!EnumDisplaySettingsExW(gdi_name.c_str(), ENUM_CURRENT_SETTINGS, &cur_dm, 0)) {
        return Result::failure("Failed to query current display settings");
    }

    DEVMODEW new_dm = cur_dm;

    if (change.width.has_value() && change.height.has_value()) {
        new_dm.dmPelsWidth = *change.width;
        new_dm.dmPelsHeight = *change.height;
        new_dm.dmFields |= (DM_PELSWIDTH | DM_PELSHEIGHT);
    }
    if (change.refresh_rate.has_value()) {
        new_dm.dmDisplayFrequency = static_cast<DWORD>(std::round(*change.refresh_rate));
        new_dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    if (change.orientation.has_value()) {
        switch (*change.orientation) {
            case DisplayOrientation::Normal: new_dm.dmDisplayOrientation = DMDO_DEFAULT; break;
            case DisplayOrientation::Rotate90: new_dm.dmDisplayOrientation = DMDO_90; break;
            case DisplayOrientation::Rotate180: new_dm.dmDisplayOrientation = DMDO_180; break;
            case DisplayOrientation::Rotate270: new_dm.dmDisplayOrientation = DMDO_270; break;
        }
        new_dm.dmFields |= DM_DISPLAYORIENTATION;
    }
    if (change.x.has_value() && change.y.has_value()) {
        new_dm.dmPosition.x = *change.x;
        new_dm.dmPosition.y = *change.y;
        new_dm.dmFields |= DM_POSITION;
    }

    LONG test_res = ChangeDisplaySettingsExW(gdi_name.c_str(), &new_dm, nullptr, CDS_TEST, nullptr);
    if (test_res != DISP_CHANGE_SUCCESSFUL) {
        return Result::failure("Display settings mode unsupported by display driver (code " + std::to_string(test_res) + ")");
    }

    LONG apply_res = ChangeDisplaySettingsExW(gdi_name.c_str(), &new_dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (apply_res != DISP_CHANGE_SUCCESSFUL) {
        return Result::failure("Failed to apply display configuration (code " + std::to_string(apply_res) + ")");
    }

    return Result::success();
}

Result WinDisplayService::apply_temporary_configuration(
    const DisplayConfigChange& change,
    std::chrono::milliseconds timeout) {
    DpiScope dpi;
    auto snap = snapshot();
    const auto* d = change.display_id.empty() ? snap.primary_display() : snap.find_display(change.display_id);
    if (!d) {
        return Result::failure("Display not found: " + change.display_id);
    }

    std::wstring gdi_name = utf8_to_wide(d->device_name);
    if (gdi_name.empty()) {
        return Result::failure("No GDI device drives display " + d->id);
    }

    DEVMODEW orig_dm{};
    orig_dm.dmSize = sizeof(orig_dm);
    if (!EnumDisplaySettingsExW(gdi_name.c_str(), ENUM_CURRENT_SETTINGS, &orig_dm, 0)) {
        return Result::failure("Failed to query original display settings for revert");
    }

    DEVMODEW target_dm = orig_dm;
    bool found_mode = false;
    if (change.width.has_value() && change.height.has_value()) {
        DEVMODEW enum_dm{};
        enum_dm.dmSize = sizeof(enum_dm);
        for (DWORD i = 0; EnumDisplaySettingsExW(gdi_name.c_str(), i, &enum_dm, 0); ++i) {
            if (enum_dm.dmPelsWidth == *change.width && enum_dm.dmPelsHeight == *change.height) {
                if (!change.refresh_rate.has_value() ||
                    std::abs(static_cast<double>(enum_dm.dmDisplayFrequency) - *change.refresh_rate) < 2.0) {
                    target_dm = enum_dm;
                    found_mode = true;
                    break;
                }
            }
        }
    }

    if (!found_mode) {
        if (change.width.has_value()) {
            target_dm.dmPelsWidth = *change.width;
            target_dm.dmFields |= DM_PELSWIDTH;
        }
        if (change.height.has_value()) {
            target_dm.dmPelsHeight = *change.height;
            target_dm.dmFields |= DM_PELSHEIGHT;
        }
        if (change.refresh_rate.has_value()) {
            target_dm.dmDisplayFrequency = static_cast<DWORD>(std::round(*change.refresh_rate));
            target_dm.dmFields |= DM_DISPLAYFREQUENCY;
        }
    }

    target_dm.dmPosition = orig_dm.dmPosition;
    target_dm.dmFields |= DM_POSITION;

    // 1. Try modern CCD SetDisplayConfig
    std::string ccd_error;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG sdc_err = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (sdc_err == ERROR_SUCCESS && pathCount > 0 && modeCount > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> orig_paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> orig_modes(modeCount);
        sdc_err = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, orig_paths.data(), &modeCount, orig_modes.data(), nullptr);
        if (sdc_err == ERROR_SUCCESS) {
            auto new_paths = orig_paths;
            auto new_modes = orig_modes;
            bool target_found = false;

            for (auto& path : new_paths) {
                DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
                tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tn.header.size = sizeof(tn);
                tn.header.adapterId = path.targetInfo.adapterId;
                tn.header.id = path.targetInfo.id;
                DisplayConfigGetDeviceInfo(&tn.header);
                std::string target_path = wide_to_utf8(tn.monitorDevicePath);

                if (target_path == d->id || new_paths.size() == 1) {
                    target_found = true;
                    if (path.sourceInfo.modeInfoIdx < new_modes.size()) {
                        auto& sm = new_modes[path.sourceInfo.modeInfoIdx].sourceMode;
                        if (change.width.has_value()) sm.width = *change.width;
                        if (change.height.has_value()) sm.height = *change.height;
                        if (change.x.has_value()) sm.position.x = *change.x;
                        if (change.y.has_value()) sm.position.y = *change.y;
                    }
                    if (path.targetInfo.modeInfoIdx < new_modes.size()) {
                        auto& tm = new_modes[path.targetInfo.modeInfoIdx].targetMode;
                        if (change.refresh_rate.has_value()) {
                            tm.targetVideoSignalInfo.vSyncFreq.Numerator = static_cast<UINT32>(std::round(*change.refresh_rate * 1000.0));
                            tm.targetVideoSignalInfo.vSyncFreq.Denominator = 1000;
                        }
                    }
                    if (change.orientation.has_value()) {
                        switch (*change.orientation) {
                            case DisplayOrientation::Normal: path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY; break;
                            case DisplayOrientation::Rotate90: path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_ROTATE90; break;
                            case DisplayOrientation::Rotate180: path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_ROTATE180; break;
                            case DisplayOrientation::Rotate270: path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_ROTATE270; break;
                        }
                    }
                    break;
                }
            }

            if (target_found) {
                sdc_err = SetDisplayConfig(
                    static_cast<UINT32>(new_paths.size()), new_paths.data(),
                    static_cast<UINT32>(new_modes.size()), new_modes.data(),
                    SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
                if (sdc_err == ERROR_SUCCESS) {
                    // The test configuration was applied without
                    // SDC_SAVE_TO_DATABASE, so the database still holds the
                    // original: a rollback only has to re-apply it, and a
                    // confirmation saves what is active now.
                    revert_manager_->start_temporary(
                        d->id, timeout,
                        [orig_paths, orig_modes]() {
                            auto p = orig_paths;
                            auto m = orig_modes;
                            LONG err = SetDisplayConfig(
                                static_cast<UINT32>(p.size()), p.data(),
                                static_cast<UINT32>(m.size()), m.data(),
                                SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
                            if (err != ERROR_SUCCESS) {
                                return Result::failure("SetDisplayConfig failed to restore the original configuration (error " +
                                                       std::to_string(err) + ")");
                            }
                            return Result::success();
                        },
                        [] { return persist_active_configuration(); });
                    return Result::success();
                }
                ccd_error = "SetDisplayConfig error " + std::to_string(sdc_err);
            } else {
                ccd_error = "no CCD path drives " + d->id;
            }
        }
    }

    // 2. Fall back to ChangeDisplaySettingsExW. The first two forms change the
    // mode for the session only; a driver that accepts neither gets the mode
    // through the registry, which the rollback then has to restore too.
    bool wrote_registry = false;
    LONG apply_res = ChangeDisplaySettingsExW(gdi_name.c_str(), &target_dm, nullptr, CDS_FULLSCREEN, nullptr);
    if (apply_res != DISP_CHANGE_SUCCESSFUL) {
        apply_res = ChangeDisplaySettingsExW(gdi_name.c_str(), &target_dm, nullptr, 0, nullptr);
    }
    if (apply_res != DISP_CHANGE_SUCCESSFUL) {
        apply_res = ChangeDisplaySettingsExW(gdi_name.c_str(), &target_dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (apply_res == DISP_CHANGE_SUCCESSFUL) {
            wrote_registry = true;
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        }
    }
    if (apply_res != DISP_CHANGE_SUCCESSFUL) {
        return Result::failure("Failed to apply temporary display settings (code " + std::to_string(apply_res) + ")" +
                               (ccd_error.empty() ? "" : "; CCD: " + ccd_error));
    }

    // Start revert timer with rollback lambda
    revert_manager_->start_temporary(
        d->id, timeout,
        [gdi_name, orig_dm, wrote_registry]() {
            DEVMODEW restore_dm = orig_dm;
            LONG rc = ChangeDisplaySettingsExW(gdi_name.c_str(), &restore_dm, nullptr,
                                               wrote_registry ? CDS_UPDATEREGISTRY : 0, nullptr);
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
            if (rc != DISP_CHANGE_SUCCESSFUL) {
                return Result::failure("ChangeDisplaySettingsExW failed to restore the original mode (code " +
                                       std::to_string(rc) + ")");
            }
            return Result::success();
        },
        [gdi_name, target_dm]() {
            // Record the active test mode as the display's registry mode
            // without switching again.
            DEVMODEW keep = target_dm;
            LONG rc = ChangeDisplaySettingsExW(gdi_name.c_str(), &keep, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET,
                                               nullptr);
            if (rc != DISP_CHANGE_SUCCESSFUL) {
                return Result::failure("ChangeDisplaySettingsExW could not save the mode to the registry (code " +
                                       std::to_string(rc) + ")");
            }
            return Result::success();
        });

    return Result::success();
}

Result WinDisplayService::confirm_configuration() {
    return revert_manager_->confirm();
}

Result WinDisplayService::revert_configuration() {
    return revert_manager_->revert();
}

bool WinDisplayService::is_revert_pending() const {
    return revert_manager_->is_pending();
}

Result WinDisplayService::set_night_light(bool enabled, uint32_t temperature_kelvin) {
    // The gamma ramp lives on the GDI device ("\\.\DISPLAYn"), which is also
    // the key snapshot() reads night-light status back under; the display id
    // is the monitor device path, which CreateDC does not accept.
    auto snap = snapshot();
    const auto* primary = snap.primary_display();
    if (!primary || primary->device_name.empty()) {
        return Result::failure("No display with a GDI device to apply a gamma ramp to");
    }
    return gamma_.set_night_light(primary->device_name, enabled, temperature_kelvin);
}

} // namespace brodisplays
