#include "x11_backend.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

// Catches X protocol errors for the duration of a request batch instead of
// letting Xlib's default handler exit the process. The handler is process
// global, so traps are serialized.
std::mutex g_trap_mutex;
int g_trapped_error = 0;

int trap_handler(Display*, XErrorEvent* e) {
    if (g_trapped_error == 0) g_trapped_error = e->error_code;
    return 0;
}

class XErrorTrap {
public:
    explicit XErrorTrap(Display* d) : lock_(g_trap_mutex), d_(d) {
        XSync(d_, False);
        g_trapped_error = 0;
        old_ = XSetErrorHandler(trap_handler);
    }
    int finish() {
        if (done_) return g_trapped_error;
        XSync(d_, False);
        XSetErrorHandler(old_);
        done_ = true;
        return g_trapped_error;
    }
    ~XErrorTrap() { finish(); }

private:
    std::lock_guard<std::mutex> lock_;
    Display* d_;
    XErrorHandler old_ = nullptr;
    bool done_ = false;
};

double calculate_refresh_rate(const XRRModeInfo& m) {
    if (m.hTotal == 0 || m.vTotal == 0) return 60.0;
    double rr = static_cast<double>(m.dotClock) / static_cast<double>(m.hTotal * m.vTotal);
    return std::round(rr * 100.0) / 100.0;
}

} // namespace

namespace {

// XOpenDisplay waits for the server's setup reply without a timeout, so a
// wedged or half-started X server would hang it. For local displays, do the
// handshake ourselves first with a bounded wait: any reply (success, failure,
// or an auth challenge) proves the server is answering.
bool local_x_server_answers(std::chrono::milliseconds timeout) {
    const char* env = std::getenv("DISPLAY");
    if (!env || !*env) return false;
    std::string disp = env;
    if (disp.rfind("unix:", 0) == 0) disp = disp.substr(4);
    if (disp.empty() || disp[0] != ':') return true; // remote/TCP: leave it to Xlib
    std::string num = disp.substr(1, disp.find('.') == std::string::npos ? std::string::npos : disp.find('.') - 1);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return true;
    const std::string path = "/tmp/.X11-unix/X" + num;

    auto try_connect = [&](bool abstract) -> int {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        socklen_t len;
        if (abstract) {
            addr.sun_path[0] = '\0';
            std::memcpy(addr.sun_path + 1, path.data(), path.size());
            len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + path.size());
        } else {
            std::memcpy(addr.sun_path, path.data(), path.size());
            len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        }
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 && errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        return fd;
    };
    int fd = try_connect(true);
    if (fd < 0) fd = try_connect(false);
    if (fd < 0) return false;

    // Connection setup: little-endian, protocol 11.0, no authorization.
    const unsigned char setup[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bool ok = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t sent = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd p{fd, static_cast<short>(sent < sizeof(setup) ? POLLOUT : POLLIN), 0};
        int r = poll(&p, 1, static_cast<int>(std::max<long long>(1, left.count())));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || (p.revents & (POLLERR | POLLNVAL))) break;
        if (sent < sizeof(setup)) {
            ssize_t n = send(fd, setup + sent, sizeof(setup) - sent, MSG_NOSIGNAL);
            if (n < 0 && errno != EAGAIN && errno != EINTR) break;
            if (n > 0) sent += static_cast<size_t>(n);
            continue;
        }
        unsigned char status = 0xff;
        ssize_t n = recv(fd, &status, 1, 0);
        ok = n == 1 && status <= 2;
        break;
    }
    close(fd);
    return ok;
}

} // namespace

bool X11Backend::is_available() {
    // Generous: some servers (Xvfb) take over a second per connection setup.
    if (!local_x_server_answers(std::chrono::milliseconds(5000))) return false;
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

    // The CRTC's new footprint, rotated, and the screen size every active
    // CRTC then needs. A CRTC may not extend past the screen, so the screen
    // grows before the CRTC change and shrinks after it.
    uint32_t mode_w = 0, mode_h = 0;
    for (int r = 0; r < res->nmode; ++r) {
        if (res->modes[r].id == target_mode) {
            mode_w = res->modes[r].width;
            mode_h = res->modes[r].height;
        }
    }
    if (target_rot & (RR_Rotate_90 | RR_Rotate_270)) std::swap(mode_w, mode_h);
    int need_w = target_x + static_cast<int>(mode_w);
    int need_h = target_y + static_cast<int>(mode_h);
    for (int c = 0; c < res->ncrtc; ++c) {
        if (res->crtcs[c] == target_out_info->crtc) continue;
        XRRCrtcInfo* other = XRRGetCrtcInfo(dpy_, res, res->crtcs[c]);
        if (!other) continue;
        if (other->mode != None) {
            need_w = std::max(need_w, other->x + static_cast<int>(other->width));
            need_h = std::max(need_h, other->y + static_cast<int>(other->height));
        }
        XRRFreeCrtcInfo(other);
    }
    // The real current screen size. DisplayWidth()/DisplayHeight() are Xlib's
    // cache, refreshed only by XRRUpdateConfiguration on this connection's
    // events, which this connection never reads; after any earlier change they
    // are stale, and a stale size either skips a needed grow (BadMatch) or a
    // shrink (the screen stays larger than the outputs).
    const int screen = DefaultScreen(dpy_);
    XWindowAttributes root_attrs{};
    XGetWindowAttributes(dpy_, root, &root_attrs);
    const int cur_w = root_attrs.width > 0 ? root_attrs.width : DisplayWidth(dpy_, screen);
    const int cur_h = root_attrs.height > 0 ? root_attrs.height : DisplayHeight(dpy_, screen);
    auto resize_screen = [&](int w, int h) {
        // Keep the physical DPI: scale the mm size with the pixel size (the
        // cached px/mm ratio is still the right ratio).
        int mm_w = std::max(1, DisplayWidthMM(dpy_, screen) * w / std::max(1, DisplayWidth(dpy_, screen)));
        int mm_h = std::max(1, DisplayHeightMM(dpy_, screen) * h / std::max(1, DisplayHeight(dpy_, screen)));
        XRRSetScreenSize(dpy_, root, w, h, mm_w, mm_h);
    };

    // Clone outputs on the same CRTC stay attached.
    std::vector<RROutput> outputs(crtc->outputs, crtc->outputs + crtc->noutput);
    if (std::find(outputs.begin(), outputs.end(), target_output) == outputs.end()) {
        outputs.push_back(target_output);
    }

    XErrorTrap trap(dpy_);
    if (need_w > cur_w || need_h > cur_h) {
        resize_screen(std::max(need_w, cur_w), std::max(need_h, cur_h));
    }
    Status s = XRRSetCrtcConfig(
        dpy_, res, target_out_info->crtc, CurrentTime,
        target_x, target_y, target_mode, target_rot,
        outputs.data(), static_cast<int>(outputs.size()));
    if (s == RRSetConfigSuccess && (need_w != cur_w || need_h != cur_h)) {
        resize_screen(need_w, need_h);
    }
    int x_error = trap.finish();

    // Read the CRTC back: Xwayland, for one, acknowledges RRSetCrtcConfig but
    // only emulates the mode for the requesting client's fullscreen windows,
    // so the real output never changes. Report that instead of success.
    bool took_effect = true;
    if (s == RRSetConfigSuccess && x_error == 0) {
        XRRScreenResources* after_res = XRRGetScreenResourcesCurrent(dpy_, root);
        if (after_res) {
            XRRCrtcInfo* after = XRRGetCrtcInfo(dpy_, after_res, target_out_info->crtc);
            if (after) {
                took_effect = after->mode == target_mode && after->x == target_x && after->y == target_y &&
                              after->rotation == target_rot;
                XRRFreeCrtcInfo(after);
            }
            XRRFreeScreenResources(after_res);
        }
    }

    XRRFreeCrtcInfo(crtc);
    XRRFreeOutputInfo(target_out_info);
    XRRFreeScreenResources(res);

    if (s != RRSetConfigSuccess) {
        return Result::failure("XRRSetCrtcConfig failed with status " + std::to_string(s));
    }
    if (x_error != 0) {
        return Result::failure("X server rejected the mode change (X error code " + std::to_string(x_error) + ")");
    }
    if (!took_effect) {
        return Result::failure("X server accepted the mode change but the output did not change "
                               "(Xwayland only emulates RandR mode changes)");
    }

    return Result::success();
}

// The watcher owns a second connection and never blocks inside Xlib: it polls
// the connection fd together with a self-pipe, and only calls XNextEvent for
// events already queued. stop_watcher() writes the pipe and joins. (The old
// design blocked in XNextEvent and tried to wake it with an XSendEvent from
// another thread that no client had selected for, so the join hung forever.)
void X11Backend::start_watcher(std::function<void()> on_change) {
    if (watching_) return;
    event_dpy_ = XOpenDisplay(nullptr);
    if (!event_dpy_) return;
    if (pipe(wake_pipe_) != 0) {
        wake_pipe_[0] = wake_pipe_[1] = -1;
        XCloseDisplay(event_dpy_);
        event_dpy_ = nullptr;
        return;
    }
    fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);

    XRRSelectInput(event_dpy_, DefaultRootWindow(event_dpy_),
                   RRScreenChangeNotifyMask | RROutputChangeNotifyMask | RRCrtcChangeNotifyMask);
    XFlush(event_dpy_);

    on_change_ = std::move(on_change);
    watching_ = true;
    watcher_thread_ = std::thread(&X11Backend::watcher_thread_func, this);
}

void X11Backend::stop_watcher() {
    if (!watching_) return;
    watching_ = false;
    char b = 1;
    (void)!write(wake_pipe_[1], &b, 1);
    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }
    close(wake_pipe_[0]);
    close(wake_pipe_[1]);
    wake_pipe_[0] = wake_pipe_[1] = -1;
    if (event_dpy_) {
        XCloseDisplay(event_dpy_);
        event_dpy_ = nullptr;
    }
}

void X11Backend::watcher_thread_func() {
    int rr_event_base = 0, rr_error_base = 0;
    XRRQueryExtension(event_dpy_, &rr_event_base, &rr_error_base);
    const int xfd = ConnectionNumber(event_dpy_);

    while (watching_) {
        bool changed = false;
        while (XPending(event_dpy_) > 0) {
            XEvent ev;
            XNextEvent(event_dpy_, &ev);
            XRRUpdateConfiguration(&ev);
            if (ev.type >= rr_event_base && ev.type < rr_event_base + RRNumberEvents) {
                changed = true;
            }
        }
        if (changed && on_change_) {
            on_change_();
        }

        pollfd fds[2] = {{xfd, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
        int r = poll(fds, 2, -1);
        if (r < 0 && errno != EINTR) break;
        if (fds[1].revents & POLLIN) break;
        if (fds[0].revents & (POLLERR | POLLHUP)) break;
    }
}

} // namespace brodisplays
