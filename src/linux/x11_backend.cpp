#include "x11_backend.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

#include "brodisplays/edid.h"

namespace brodisplays {

namespace {

DisplayOrientation xrr_rotation_to_orientation(Rotation rotation) {
    if (rotation & RR_Rotate_90) return DisplayOrientation::Rotate90;
    if (rotation & RR_Rotate_180) return DisplayOrientation::Rotate180;
    if (rotation & RR_Rotate_270) return DisplayOrientation::Rotate270;
    return DisplayOrientation::Normal;
}

Rotation orientation_to_xrr_rotation(DisplayOrientation o) {
    switch (o) {
        case DisplayOrientation::Normal: return RR_Rotate_0;
        case DisplayOrientation::Rotate90: return RR_Rotate_90;
        case DisplayOrientation::Rotate180: return RR_Rotate_180;
        case DisplayOrientation::Rotate270: return RR_Rotate_270;
    }
    return RR_Rotate_0;
}

double calculate_refresh_rate(const XRRModeInfo& m) {
    if (m.hTotal == 0 || m.vTotal == 0) return 60.0;
    double rr = static_cast<double>(m.dotClock) / static_cast<double>(m.hTotal * m.vTotal);
    return std::round(rr * 100.0) / 100.0;
}

} // namespace

bool X11Backend::is_available() {
    Display* d = XOpenDisplay(nullptr);
    if (!d) return false;
    int event_base = 0, error_base = 0;
    bool ok = XRRQueryExtension(d, &event_base, &error_base);
    XCloseDisplay(d);
    return ok;
}

std::unique_ptr<X11Backend> X11Backend::create(std::string* error) {
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        if (error) *error = "Failed to open X11 display (check DISPLAY environment variable)";
        return nullptr;
    }

    int event_base = 0, error_base = 0;
    if (!XRRQueryExtension(d, &event_base, &error_base)) {
        XCloseDisplay(d);
        if (error) *error = "XRandR extension not supported by X server";
        return nullptr;
    }

    return std::unique_ptr<X11Backend>(new X11Backend(d));
}

X11Backend::X11Backend(Display* dpy)
    : dpy_(dpy) {}

X11Backend::~X11Backend() {
    stop_watcher();
    if (dpy_) {
        XCloseDisplay(dpy_);
        dpy_ = nullptr;
    }
}

std::vector<DisplayInfo> X11Backend::snapshot() {
    std::lock_guard<std::mutex> lock(dpy_mutex_);
    std::vector<DisplayInfo> results;
    if (!dpy_) return results;

    Window root = DefaultRootWindow(dpy_);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy_, root);
    if (!res) return results;

    RROutput primary = XRRGetOutputPrimary(dpy_, root);
    Atom edid_atom = XInternAtom(dpy_, "EDID", True);
    if (edid_atom == None) {
        edid_atom = XInternAtom(dpy_, RR_PROPERTY_RANDR_EDID, True);
    }

    for (int i = 0; i < res->noutput; ++i) {
        RROutput out_id = res->outputs[i];
        XRROutputInfo* out_info = XRRGetOutputInfo(dpy_, res, out_id);
        if (!out_info) continue;

        if (out_info->connection != RR_Connected) {
            XRRFreeOutputInfo(out_info);
            continue;
        }

        DisplayInfo info;
        info.id = out_info->name ? out_info->name : ("Output-" + std::to_string(out_id));
        info.name = info.id;
        info.model = info.id;
        info.adapter_name = "X11 Display Device";
        info.is_connected = true;
        info.is_primary = (out_id == primary) || (results.empty());
        info.physical_width_mm = static_cast<uint32_t>(out_info->mm_width);
        info.physical_height_mm = static_cast<uint32_t>(out_info->mm_height);

        if (info.id.find("eDP") != std::string::npos ||
            info.id.find("LVDS") != std::string::npos ||
            info.id.find("DSI") != std::string::npos) {
            info.is_internal = true;
        }

        // CRTC info (current mode and geometry)
        if (out_info->crtc != 0) {
            XRRCrtcInfo* crtc = XRRGetCrtcInfo(dpy_, res, out_info->crtc);
            if (crtc) {
                info.geometry.x = crtc->x;
                info.geometry.y = crtc->y;
                info.geometry.width = crtc->width;
                info.geometry.height = crtc->height;
                info.orientation = xrr_rotation_to_orientation(crtc->rotation);

                // Find mode
                for (int m = 0; m < res->nmode; ++m) {
                    if (res->modes[m].id == crtc->mode) {
                        info.current_mode = DisplayMode{
                            res->modes[m].width,
                            res->modes[m].height,
                            calculate_refresh_rate(res->modes[m])};
                        break;
                    }
                }
                XRRFreeCrtcInfo(crtc);
            }
        }

        // Available modes
        for (int m = 0; m < out_info->nmode; ++m) {
            RRMode mode_id = out_info->modes[m];
            for (int r = 0; r < res->nmode; ++r) {
                if (res->modes[r].id == mode_id) {
                    DisplayMode dm{
                        res->modes[r].width,
                        res->modes[r].height,
                        calculate_refresh_rate(res->modes[r])};

                    bool exists = false;
                    for (const auto& ex : info.available_modes) {
                        if (ex == dm) { exists = true; break; }
                    }
                    if (!exists) {
                        info.available_modes.push_back(dm);
                    }
                    break;
                }
            }
        }

        // Query EDID property
        if (edid_atom != None) {
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char* prop_data = nullptr;

            if (XRRGetOutputProperty(
                    dpy_, out_id, edid_atom, 0, 100, False, False,
                    AnyPropertyType, &actual_type, &actual_format,
                    &nitems, &bytes_after, &prop_data) == Success && prop_data) {
                if (nitems >= 128) {
                    if (parse_edid(prop_data, nitems, info.edid)) {
                        if (!info.edid.monitor_name.empty()) {
                            info.name = info.edid.monitor_name;
                            info.model = info.edid.monitor_name;
                        }
                        if (!info.edid.manufacturer_id.empty()) {
                            info.manufacturer = info.edid.manufacturer_id;
                        }
                    }
                }
                XFree(prop_data);
            }
        }

        info.scale.factor = 1.0;
        info.scale.dpi = 96;

        results.push_back(info);
        XRRFreeOutputInfo(out_info);
    }

    XRRFreeScreenResources(res);
    return results;
}

Result X11Backend::apply_configuration(const DisplayConfigChange& change) {
    std::lock_guard<std::mutex> lock(dpy_mutex_);
    if (!dpy_) return Result::failure("X11 display not open");

    Window root = DefaultRootWindow(dpy_);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy_, root);
    if (!res) return Result::failure("Failed to get XRandR screen resources");

    RROutput target_output = 0;
    XRROutputInfo* target_out_info = nullptr;

    for (int i = 0; i < res->noutput; ++i) {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy_, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->name && (change.display_id.empty() || change.display_id == oi->name)) {
            target_output = res->outputs[i];
            target_out_info = oi;
            break;
        }
        XRRFreeOutputInfo(oi);
    }

    if (!target_out_info || target_out_info->crtc == 0) {
        if (target_out_info) XRRFreeOutputInfo(target_out_info);
        XRRFreeScreenResources(res);
        return Result::failure("Target display output or CRTC not found: " + change.display_id);
    }

    XRRCrtcInfo* crtc = XRRGetCrtcInfo(dpy_, res, target_out_info->crtc);
    if (!crtc) {
        XRRFreeOutputInfo(target_out_info);
        XRRFreeScreenResources(res);
        return Result::failure("Failed to get CRTC info for output");
    }

    // Determine target mode
    RRMode target_mode = crtc->mode;
    if (change.width.has_value() && change.height.has_value()) {
        bool mode_found = false;
        for (int m = 0; m < target_out_info->nmode; ++m) {
            RRMode mid = target_out_info->modes[m];
            for (int r = 0; r < res->nmode; ++r) {
                if (res->modes[r].id == mid &&
                    res->modes[r].width == *change.width &&
                    res->modes[r].height == *change.height) {
                    if (!change.refresh_rate.has_value() ||
                        std::abs(calculate_refresh_rate(res->modes[r]) - *change.refresh_rate) < 1.0) {
                        target_mode = mid;
                        mode_found = true;
                        break;
                    }
                }
            }
            if (mode_found) break;
        }
        if (!mode_found) {
            XRRFreeCrtcInfo(crtc);
            XRRFreeOutputInfo(target_out_info);
            XRRFreeScreenResources(res);
            return Result::failure("Requested resolution not found in output modes");
        }
    }

    int target_x = change.x.value_or(crtc->x);
    int target_y = change.y.value_or(crtc->y);
    Rotation target_rot = change.orientation.has_value()
        ? orientation_to_xrr_rotation(*change.orientation)
        : crtc->rotation;

    Status s = XRRSetCrtcConfig(
        dpy_, res, target_out_info->crtc, CurrentTime,
        target_x, target_y, target_mode, target_rot,
        &target_output, 1);

    XSync(dpy_, False);
    XRRFreeCrtcInfo(crtc);
    XRRFreeOutputInfo(target_out_info);
    XRRFreeScreenResources(res);

    if (s != RRSetConfigSuccess) {
        return Result::failure("XRRSetCrtcConfig failed with status " + std::to_string(s));
    }

    return Result::success();
}

void X11Backend::start_watcher(std::function<void()> on_change) {
    if (watching_) return;
    on_change_ = std::move(on_change);
    watching_ = true;
    watcher_thread_ = std::thread(&X11Backend::watcher_thread_func, this);
}

void X11Backend::stop_watcher() {
    if (!watching_) return;
    watching_ = false;

    if (event_dpy_) {
        // Send dummy client message to root to wake XNextEvent
        Window root = DefaultRootWindow(event_dpy_);
        XEvent ev{};
        ev.type = ClientMessage;
        ev.xclient.window = root;
        ev.xclient.message_type = XInternAtom(event_dpy_, "BRO_STOP", False);
        ev.xclient.format = 32;
        XSendEvent(event_dpy_, root, False, StructureNotifyMask, &ev);
        XFlush(event_dpy_);
    }

    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }

    if (event_dpy_) {
        XCloseDisplay(event_dpy_);
        event_dpy_ = nullptr;
    }
}

void X11Backend::watcher_thread_func() {
    event_dpy_ = XOpenDisplay(nullptr);
    if (!event_dpy_) return;

    Window root = DefaultRootWindow(event_dpy_);
    XRRSelectInput(event_dpy_, root, RROutputChangeNotifyMask | RRCrtcChangeNotifyMask);

    int rr_event_base = 0, rr_error_base = 0;
    XRRQueryExtension(event_dpy_, &rr_event_base, &rr_error_base);

    while (watching_) {
        XEvent ev;
        XNextEvent(event_dpy_, &ev);
        if (!watching_) break;

        if (ev.type >= rr_event_base && ev.type < rr_event_base + RRNumberEvents) {
            if (on_change_) {
                on_change_();
            }
        }
    }
}

} // namespace brodisplays
