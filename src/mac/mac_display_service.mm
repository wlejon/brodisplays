#import "mac_display_service.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ColorSync/ColorSync.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/graphics/IOGraphicsLib.h>

#include <cmath>
#include <vector>

#include "brodisplays/edid.h"

namespace brodisplays {

namespace {

std::string cfstring_to_utf8(CFStringRef cfstr) {
    if (!cfstr) return "";
    const char* ptr = CFStringGetCStringPtr(cfstr, kCFStringEncodingUTF8);
    if (ptr) return std::string(ptr);

    CFIndex len = CFStringGetLength(cfstr);
    CFIndex max_size = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(max_size, 0);
    if (CFStringGetCString(cfstr, &out[0], max_size, kCFStringEncodingUTF8)) {
        out.resize(std::strlen(out.c_str()));
        return out;
    }
    return "";
}


std::string query_display_name_iokit(CGDirectDisplayID display_id, bool is_internal) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplayConnect"), &it) != kIOReturnSuccess) {
        return is_internal ? "Color LCD" : "Display " + std::to_string(display_id);
    }

    io_service_t service;
    std::string name;
    while ((service = IOIteratorNext(it)) != 0) {
        CFDictionaryRef info = IODisplayCreateInfoDictionary(service, kIODisplayOnlyPreferredName);
        if (info) {
            CFDictionaryRef names = (CFDictionaryRef)CFDictionaryGetValue(info, CFSTR(kDisplayProductName));
            if (names) {
                CFIndex count = CFDictionaryGetCount(names);
                if (count > 0) {
                    const void** values = (const void**)alloca(count * sizeof(void*));
                    CFDictionaryGetKeysAndValues(names, nullptr, values);
                    name = cfstring_to_utf8((CFStringRef)values[0]);
                }
            }
            CFRelease(info);
        }
        IOObjectRelease(service);
        if (!name.empty()) break;
    }
    IOObjectRelease(it);

    if (name.empty()) {
        name = is_internal ? "Color LCD" : "Display " + std::to_string(display_id);
    }
    return name;
}

void query_edid_iokit(CGDirectDisplayID /*display_id*/, EdidInfo& out_edid) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplayConnect"), &it) != kIOReturnSuccess) {
        return;
    }

    io_service_t service;
    while ((service = IOIteratorNext(it)) != 0) {
        CFDictionaryRef info = IODisplayCreateInfoDictionary(service, kIODisplayMatchingInfo);
        if (info) {
            CFDataRef edidData = (CFDataRef)CFDictionaryGetValue(info, CFSTR(kIODisplayEDIDKey));
            if (edidData) {
                const uint8_t* bytes = CFDataGetBytePtr(edidData);
                CFIndex len = CFDataGetLength(edidData);
                if (bytes && len >= 128) {
                    parse_edid(bytes, len, out_edid);
                }
            }
            CFRelease(info);
        }
        IOObjectRelease(service);
        if (!out_edid.raw_bytes.empty()) break;
    }
    IOObjectRelease(it);
}

} // namespace

std::unique_ptr<DisplayService> DisplayService::create(
    const DisplayServiceConfig& config,
    std::string* error) {
    try {
        return std::make_unique<MacDisplayService>(config);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

MacDisplayService::MacDisplayService(const DisplayServiceConfig& config)
    : config_(config) {
    revert_manager_ = std::make_unique<RevertManager>(events_);

    if (config_.enable_events) {
        watcher_ = std::make_unique<MacDisplayWatcher>([this] {
            on_system_display_change();
        });
    }

    last_snapshot_ = snapshot();
}

MacDisplayService::~MacDisplayService() {
    watcher_.reset();
    revert_manager_.reset();
    gamma_.restore_all();
}

MessageQueue<DisplayEvent>& MacDisplayService::events() {
    return events_;
}

void MacDisplayService::on_system_display_change() {
    auto new_snap = snapshot();
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        last_snapshot_ = new_snap;
    }
    events_.push(DisplaysChanged{new_snap});
}

DisplaysSnapshot MacDisplayService::snapshot() const {
    DisplaysSnapshot snap;
    snap.timestamp = std::chrono::system_clock::now();

    uint32_t count = 0;
    CGGetOnlineDisplayList(0, nullptr, &count);
    if (count == 0) return snap;

    std::vector<CGDirectDisplayID> displays(count);
    CGGetOnlineDisplayList(count, displays.data(), &count);

    CGDirectDisplayID main_display = CGMainDisplayID();

    for (uint32_t i = 0; i < count; ++i) {
        CGDirectDisplayID d = displays[i];

        DisplayInfo info;
        info.id = std::to_string(d);
        info.is_primary = (d == main_display);
        info.is_internal = CGDisplayIsBuiltin(d) != 0;
        info.name = query_display_name_iokit(d, info.is_internal);
        info.adapter_name = "Apple GPU";
        info.manufacturer = info.is_internal ? "Apple" : "Unknown";
        info.model = info.name;
        info.is_connected = true;

        CGRect bounds = CGDisplayBounds(d);
        info.geometry.x = static_cast<int32_t>(bounds.origin.x);
        info.geometry.y = static_cast<int32_t>(bounds.origin.y);
        info.geometry.width = static_cast<uint32_t>(bounds.size.width);
        info.geometry.height = static_cast<uint32_t>(bounds.size.height);

        CGSize mmSize = CGDisplayScreenSize(d);
        info.physical_width_mm = static_cast<uint32_t>(mmSize.width);
        info.physical_height_mm = static_cast<uint32_t>(mmSize.height);

        double rot = CGDisplayRotation(d);
        if (rot >= 45.0 && rot < 135.0) info.orientation = DisplayOrientation::Rotate90;
        else if (rot >= 135.0 && rot < 225.0) info.orientation = DisplayOrientation::Rotate180;
        else if (rot >= 225.0 && rot < 315.0) info.orientation = DisplayOrientation::Rotate270;
        else info.orientation = DisplayOrientation::Normal;

        // Current mode
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(d);
        if (mode) {
            uint32_t px_width = static_cast<uint32_t>(CGDisplayModeGetPixelWidth(mode));
            uint32_t px_height = static_cast<uint32_t>(CGDisplayModeGetPixelHeight(mode));
            double rr = CGDisplayModeGetRefreshRate(mode);
            if (rr == 0.0) rr = 60.0;

            info.current_mode = DisplayMode{px_width, px_height, std::round(rr * 100.0) / 100.0};

            double scale = (bounds.size.width > 0)
                ? static_cast<double>(px_width) / bounds.size.width
                : 1.0;
            info.scale.factor = scale;
            info.scale.dpi = static_cast<int32_t>(std::round(scale * 110.0)); // Mac retina baseline ~220 DPI vs 110

            CGDisplayModeRelease(mode);
        }

        // Available modes
        CFArrayRef allModes = CGDisplayCopyAllDisplayModes(d, nullptr);
        if (allModes) {
            CFIndex numModes = CFArrayGetCount(allModes);
            for (CFIndex m = 0; m < numModes; ++m) {
                CGDisplayModeRef dm = (CGDisplayModeRef)CFArrayGetValueAtIndex(allModes, m);
                uint32_t w = static_cast<uint32_t>(CGDisplayModeGetPixelWidth(dm));
                uint32_t h = static_cast<uint32_t>(CGDisplayModeGetPixelHeight(dm));
                double rr = CGDisplayModeGetRefreshRate(dm);
                if (rr == 0.0) rr = 60.0;
                DisplayMode cand{w, h, std::round(rr * 100.0) / 100.0};

                bool found = false;
                for (const auto& ex : info.available_modes) {
                    if (ex == cand) { found = true; break; }
                }
                if (!found) {
                    info.available_modes.push_back(cand);
                }
            }
            CFRelease(allModes);
        }

        // ColorSync profile
        ColorSyncProfileRef profile = ColorSyncProfileCreateWithDisplayID(d);
        if (profile) {
            CFStringRef desc = ColorSyncProfileCopyDescriptionString(profile);
            if (desc) {
                info.color_profile.name = cfstring_to_utf8(desc);
                CFRelease(desc);
            }
            CFRelease(profile);
        }

        // EDID
        query_edid_iokit(d, info.edid);
        if (!info.edid.monitor_name.empty()) {
            info.name = info.edid.monitor_name;
        }

        // Night light
        info.night_light = gamma_.get_status(d);

        // HDR
        info.hdr.supported = info.is_internal; // MacBook Liquid Retina XDR supports HDR
        info.hdr.enabled = info.is_internal;

        snap.displays.push_back(info);
    }

    return snap;
}

Result MacDisplayService::apply_configuration(const DisplayConfigChange& change) {
    CGDirectDisplayID d = CGMainDisplayID();
    if (!change.display_id.empty()) {
        try {
            d = static_cast<CGDirectDisplayID>(std::stoul(change.display_id));
        } catch (...) {}
    }

    CFArrayRef allModes = CGDisplayCopyAllDisplayModes(d, nullptr);
    if (!allModes) {
        return Result::failure("Failed to copy display modes for display " + std::to_string(d));
    }

    CGDisplayModeRef targetMode = nullptr;
    CFIndex numModes = CFArrayGetCount(allModes);
    for (CFIndex i = 0; i < numModes; ++i) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(allModes, i);
        uint32_t w = static_cast<uint32_t>(CGDisplayModeGetPixelWidth(m));
        uint32_t h = static_cast<uint32_t>(CGDisplayModeGetPixelHeight(m));
        double rr = CGDisplayModeGetRefreshRate(m);

        bool w_ok = !change.width.has_value() || (*change.width == w);
        bool h_ok = !change.height.has_value() || (*change.height == h);
        bool rr_ok = !change.refresh_rate.has_value() || (std::abs(*change.refresh_rate - rr) < 1.0);

        if (w_ok && h_ok && rr_ok) {
            targetMode = m;
            break;
        }
    }

    if (!targetMode) {
        CFRelease(allModes);
        return Result::failure("Requested display mode not supported by macOS CoreGraphics");
    }

    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
        CFRelease(allModes);
        return Result::failure("CGBeginDisplayConfiguration failed");
    }

    CGConfigureDisplayWithDisplayMode(config, d, targetMode, nullptr);
    CGError err = CGCompleteDisplayConfiguration(config, kCGConfigurePermanently);
    CFRelease(allModes);

    if (err != kCGErrorSuccess) {
        return Result::failure("CGCompleteDisplayConfiguration failed: " + std::to_string(err));
    }

    return Result::success();
}

Result MacDisplayService::apply_temporary_configuration(
    const DisplayConfigChange& change,
    std::chrono::milliseconds timeout) {
    CGDirectDisplayID d = CGMainDisplayID();
    if (!change.display_id.empty()) {
        try {
            d = static_cast<CGDirectDisplayID>(std::stoul(change.display_id));
        } catch (...) {}
    }

    CGDisplayModeRef origMode = CGDisplayCopyDisplayMode(d);
    if (!origMode) {
        return Result::failure("Failed to capture original display mode for revert");
    }

    CFArrayRef allModes = CGDisplayCopyAllDisplayModes(d, nullptr);
    if (!allModes) {
        CGDisplayModeRelease(origMode);
        return Result::failure("Failed to copy display modes for display " + std::to_string(d));
    }

    CGDisplayModeRef targetMode = nullptr;
    CFIndex numModes = CFArrayGetCount(allModes);
    for (CFIndex i = 0; i < numModes; ++i) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(allModes, i);
        uint32_t w = static_cast<uint32_t>(CGDisplayModeGetPixelWidth(m));
        uint32_t h = static_cast<uint32_t>(CGDisplayModeGetPixelHeight(m));
        double rr = CGDisplayModeGetRefreshRate(m);

        bool w_ok = !change.width.has_value() || (*change.width == w);
        bool h_ok = !change.height.has_value() || (*change.height == h);
        bool rr_ok = !change.refresh_rate.has_value() || (std::abs(*change.refresh_rate - rr) < 1.0);

        if (w_ok && h_ok && rr_ok) {
            targetMode = m;
            break;
        }
    }

    if (!targetMode) {
        CGDisplayModeRelease(origMode);
        CFRelease(allModes);
        return Result::failure("Requested display mode not supported by CoreGraphics");
    }

    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
        CGDisplayModeRelease(origMode);
        CFRelease(allModes);
        return Result::failure("CGBeginDisplayConfiguration failed");
    }

    CGConfigureDisplayWithDisplayMode(config, d, targetMode, nullptr);
    CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    CFRelease(allModes);

    if (err != kCGErrorSuccess) {
        CGDisplayModeRelease(origMode);
        return Result::failure("CGCompleteDisplayConfiguration failed: " + std::to_string(err));
    }

    // Start revert timer with rollback lambda
    revert_manager_->start_temporary(
        std::to_string(d), timeout,
        [d, origMode]() {
            CGDisplayConfigRef cfg;
            if (CGBeginDisplayConfiguration(&cfg) == kCGErrorSuccess) {
                CGConfigureDisplayWithDisplayMode(cfg, d, origMode, nullptr);
                CGCompleteDisplayConfiguration(cfg, kCGConfigureForSession);
            }
            CGDisplayModeRelease(origMode);
        });

    return Result::success();
}

Result MacDisplayService::confirm_configuration() {
    return revert_manager_->confirm();
}

Result MacDisplayService::revert_configuration() {
    return revert_manager_->revert();
}

bool MacDisplayService::is_revert_pending() const {
    return revert_manager_->is_pending();
}

Result MacDisplayService::set_night_light(bool enabled, uint32_t temperature_kelvin) {
    uint32_t d = CGMainDisplayID();
    return gamma_.set_night_light(d, enabled, temperature_kelvin);
}

} // namespace brodisplays
