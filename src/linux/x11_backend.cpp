#include "x11_backend.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <xcb/randr.h>

#include "brodisplays/edid.h"
#include "common/color_temperature.h"

namespace brodisplays {

namespace {

template <class T>
using Reply = std::unique_ptr<T, decltype(&std::free)>;

template <class T>
Reply<T> own(T* p) {
    return Reply<T>(p, &std::free);
}

// Frees the error XCB hands back with a reply; true when there was none.
bool no_error(xcb_generic_error_t* e) {
    if (!e) return true;
    std::free(e);
    return false;
}

DisplayOrientation rotation_to_orientation(uint16_t rotation) {
    if (rotation & XCB_RANDR_ROTATION_ROTATE_90) return DisplayOrientation::Rotate90;
    if (rotation & XCB_RANDR_ROTATION_ROTATE_180) return DisplayOrientation::Rotate180;
    if (rotation & XCB_RANDR_ROTATION_ROTATE_270) return DisplayOrientation::Rotate270;
    return DisplayOrientation::Normal;
}

uint16_t orientation_to_rotation(DisplayOrientation o) {
    switch (o) {
        case DisplayOrientation::Normal: return XCB_RANDR_ROTATION_ROTATE_0;
        case DisplayOrientation::Rotate90: return XCB_RANDR_ROTATION_ROTATE_90;
        case DisplayOrientation::Rotate180: return XCB_RANDR_ROTATION_ROTATE_180;
        case DisplayOrientation::Rotate270: return XCB_RANDR_ROTATION_ROTATE_270;
    }
    return XCB_RANDR_ROTATION_ROTATE_0;
}

double refresh_of(const xcb_randr_mode_info_t& m) {
    if (m.htotal == 0 || m.vtotal == 0) return 0.0;
    double rr = static_cast<double>(m.dot_clock) / (static_cast<double>(m.htotal) * m.vtotal);
    return std::round(rr * 100.0) / 100.0;
}

const xcb_randr_mode_info_t* find_mode(const xcb_randr_mode_info_t* modes, int n, xcb_randr_mode_t id) {
    for (int i = 0; i < n; ++i) {
        if (modes[i].id == id) return &modes[i];
    }
    return nullptr;
}

// One request batch's view of the screen: resources plus each output's and
// CRTC's info, fetched with all requests in flight before the first reply.
struct ScreenState {
    Reply<xcb_randr_get_screen_resources_current_reply_t> res{nullptr, &std::free};
    const xcb_randr_mode_info_t* modes = nullptr;
    int nmodes = 0;
    std::vector<xcb_randr_output_t> outputs;
    std::vector<Reply<xcb_randr_get_output_info_reply_t>> output_info;
    std::map<xcb_randr_crtc_t, Reply<xcb_randr_get_crtc_info_reply_t>> crtcs;

    bool load(xcb_connection_t* c, xcb_window_t root) {
        xcb_generic_error_t* err = nullptr;
        res = own(xcb_randr_get_screen_resources_current_reply(
            c, xcb_randr_get_screen_resources_current(c, root), &err));
        if (!no_error(err) || !res) return false;
        modes = xcb_randr_get_screen_resources_current_modes(res.get());
        nmodes = xcb_randr_get_screen_resources_current_modes_length(res.get());
        const xcb_randr_output_t* outs = xcb_randr_get_screen_resources_current_outputs(res.get());
        outputs.assign(outs, outs + xcb_randr_get_screen_resources_current_outputs_length(res.get()));
        const xcb_randr_crtc_t* crtc_ids = xcb_randr_get_screen_resources_current_crtcs(res.get());
        const int ncrtcs = xcb_randr_get_screen_resources_current_crtcs_length(res.get());

        std::vector<xcb_randr_get_output_info_cookie_t> oc;
        for (auto o : outputs) oc.push_back(xcb_randr_get_output_info(c, o, res->config_timestamp));
        std::vector<xcb_randr_get_crtc_info_cookie_t> cc;
        for (int i = 0; i < ncrtcs; ++i) cc.push_back(xcb_randr_get_crtc_info(c, crtc_ids[i], res->config_timestamp));
        for (auto& cookie : oc) {
            output_info.push_back(own(xcb_randr_get_output_info_reply(c, cookie, &err)));
            no_error(err);
            err = nullptr;
        }
        for (int i = 0; i < ncrtcs; ++i) {
            auto r = own(xcb_randr_get_crtc_info_reply(c, cc[static_cast<size_t>(i)], &err));
            no_error(err);
            err = nullptr;
            if (r) crtcs.emplace(crtc_ids[i], std::move(r));
        }
        return !xcb_connection_has_error(c);
    }

    const xcb_randr_get_crtc_info_reply_t* crtc(xcb_randr_crtc_t id) const {
        auto it = crtcs.find(id);
        return it == crtcs.end() ? nullptr : it->second.get();
    }

    static std::string name_of(const xcb_randr_get_output_info_reply_t* oi) {
        return std::string(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(oi)),
                           static_cast<size_t>(xcb_randr_get_output_info_name_length(oi)));
    }
};

xcb_atom_t existing_atom(xcb_connection_t* c, const char* name) {
    auto r = own(xcb_intern_atom_reply(c, xcb_intern_atom(c, 1, static_cast<uint16_t>(std::strlen(name)), name),
                                       nullptr));
    return r ? r->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
}

} // namespace

std::unique_ptr<X11Backend> X11Backend::create(std::string* error) {
    auto conn = X11Connection::open(X11Connection::kIoTimeout, error);
    if (!conn) return nullptr;
    xcb_connection_t* c = conn->get();
    {
        X11Connection::Op op(*conn);
        const xcb_query_extension_reply_t* ext = xcb_get_extension_data(c, &xcb_randr_id);
        if (!ext || !ext->present) {
            if (error) *error = conn->broken() ? "X server stopped answering" : "XRandR extension not supported by X server";
            return nullptr;
        }
        auto v = own(xcb_randr_query_version_reply(c, xcb_randr_query_version(c, 1, 5), nullptr));
        if (!v || v->major_version < 1 || (v->major_version == 1 && v->minor_version < 3)) {
            if (error) *error = conn->broken() ? "X server stopped answering" : "the X server's RandR is older than 1.3";
            return nullptr;
        }
    }
    if (conn->broken()) {
        if (error) *error = "X server stopped answering";
        return nullptr;
    }
    return std::unique_ptr<X11Backend>(new X11Backend(std::move(conn)));
}

X11Backend::X11Backend(std::unique_ptr<X11Connection> conn)
    : conn_(std::move(conn)) {}

X11Backend::~X11Backend() {
    stop_watcher();
    std::lock_guard<std::mutex> lock(mutex_);
    restore_gamma_locked();
}

bool X11Backend::broken() const {
    return conn_->broken();
}

std::vector<DisplayInfo> X11Backend::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DisplayInfo> results;
    if (conn_->broken()) return results;
    xcb_connection_t* c = conn_->get();
    X11Connection::Op op(*conn_);

    ScreenState s;
    if (!s.load(c, conn_->root())) return results;
    auto primary_reply = own(xcb_randr_get_output_primary_reply(c, xcb_randr_get_output_primary(c, conn_->root()),
                                                                nullptr));
    const xcb_randr_output_t primary = primary_reply ? primary_reply->output : XCB_NONE;
    xcb_atom_t edid_atom = existing_atom(c, "EDID");

    // Gamma sizes and EDIDs of the connected outputs, all in flight at once.
    std::vector<xcb_randr_get_output_property_cookie_t> edid_cookies(s.outputs.size());
    std::map<xcb_randr_crtc_t, xcb_randr_get_crtc_gamma_size_cookie_t> gamma_cookies;
    for (size_t i = 0; i < s.outputs.size(); ++i) {
        const auto* oi = s.output_info[i].get();
        if (!oi || oi->connection != XCB_RANDR_CONNECTION_CONNECTED) continue;
        if (edid_atom != XCB_ATOM_NONE) {
            edid_cookies[i] = xcb_randr_get_output_property(c, s.outputs[i], edid_atom, XCB_ATOM_ANY, 0, 256, 0, 0);
        }
        if (oi->crtc && !gamma_cookies.count(oi->crtc)) {
            gamma_cookies.emplace(oi->crtc, xcb_randr_get_crtc_gamma_size(c, oi->crtc));
        }
    }
    std::map<xcb_randr_crtc_t, uint16_t> gamma_size;
    for (auto& [crtc, cookie] : gamma_cookies) {
        auto r = own(xcb_randr_get_crtc_gamma_size_reply(c, cookie, nullptr));
        gamma_size[crtc] = r ? r->size : 0;
    }

    struct Placed {
        xcb_randr_crtc_t crtc;
        DisplayGeometry geometry;
        std::string id;
    };
    std::vector<Placed> placed;
    for (size_t i = 0; i < s.outputs.size(); ++i) {
        const auto* oi = s.output_info[i].get();
        if (!oi || oi->connection != XCB_RANDR_CONNECTION_CONNECTED) continue;

        DisplayInfo info;
        info.id = ScreenState::name_of(oi);
        if (info.id.empty()) info.id = "Output-" + std::to_string(s.outputs[i]);
        info.device_name = info.id;
        info.name = info.id;
        info.model = info.id;
        info.adapter_name = "X11 Display Device";
        info.is_connected = true;
        info.is_primary = s.outputs[i] == primary;
        info.physical_width_mm = oi->mm_width;
        info.physical_height_mm = oi->mm_height;
        info.is_internal = info.id.find("eDP") != std::string::npos || info.id.find("LVDS") != std::string::npos ||
                           info.id.find("DSI") != std::string::npos;

        const auto* crtc = oi->crtc ? s.crtc(oi->crtc) : nullptr;
        info.is_active = crtc && crtc->mode != XCB_NONE;
        if (crtc) {
            info.geometry = DisplayGeometry{crtc->x, crtc->y, crtc->width, crtc->height};
            info.orientation = rotation_to_orientation(crtc->rotation);
            if (const auto* m = find_mode(s.modes, s.nmodes, crtc->mode)) {
                info.current_mode = DisplayMode{m->width, m->height, refresh_of(*m)};
            }
        }
        // Clones: an output on a CRTC an earlier output drives, or showing
        // exactly the same desktop area (xrandr --same-as on its own CRTC).
        if (info.is_active) {
            for (const auto& p : placed) {
                if (p.crtc == oi->crtc || p.geometry == info.geometry) {
                    info.mirror_of = p.id;
                    break;
                }
            }
            if (info.mirror_of.empty()) placed.push_back(Placed{oi->crtc, info.geometry, info.id});
        }

        const xcb_randr_mode_t* mode_ids = xcb_randr_get_output_info_modes(oi);
        for (int m = 0; m < xcb_randr_get_output_info_modes_length(oi); ++m) {
            if (const auto* mi = find_mode(s.modes, s.nmodes, mode_ids[m])) {
                DisplayMode dm{mi->width, mi->height, refresh_of(*mi)};
                if (std::find(info.available_modes.begin(), info.available_modes.end(), dm) ==
                    info.available_modes.end()) {
                    info.available_modes.push_back(dm);
                }
            }
        }

        if (edid_atom != XCB_ATOM_NONE) {
            auto prop = own(xcb_randr_get_output_property_reply(c, edid_cookies[i], nullptr));
            if (prop && prop->format == 8) {
                const int len = xcb_randr_get_output_property_data_length(prop.get());
                const uint8_t* data = xcb_randr_get_output_property_data(prop.get());
                if (len >= 128 && parse_edid(data, static_cast<size_t>(len), info.edid)) {
                    if (!info.edid.monitor_name.empty()) {
                        info.name = info.edid.monitor_name;
                        info.model = info.edid.monitor_name;
                    }
                    if (!info.edid.manufacturer_id.empty()) info.manufacturer = info.edid.manufacturer_id;
                }
            }
        }

        info.scale.factor = 1.0;
        info.scale.dpi = 96;
        info.night_light.supported = oi->crtc && gamma_size[oi->crtc] > 0;
        info.night_light.enabled = info.night_light.supported && night_light_on_;
        info.night_light.temperature_kelvin = info.night_light.enabled ? night_light_kelvin_ : 6500;

        results.push_back(std::move(info));
    }

    // Without a RandR primary output, the first display that owns desktop
    // area stands in (exactly one display is primary).
    if (std::none_of(results.begin(), results.end(), [](const DisplayInfo& d) { return d.is_primary; })) {
        for (auto& d : results) {
            if (d.is_active && d.mirror_of.empty()) {
                d.is_primary = true;
                break;
            }
        }
    }
    if (conn_->broken()) results.clear();  // abandoned mid-way: nothing here is trustworthy
    return results;
}

Result X11Backend::apply_configuration(const DisplayConfigChange& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_->broken()) return Result::failure("The X server stopped answering");
    xcb_connection_t* c = conn_->get();
    const xcb_window_t root = conn_->root();
    X11Connection::Op op(*conn_);

    ScreenState s;
    if (!s.load(c, root)) return Result::failure("Failed to get XRandR screen resources");

    size_t target = s.outputs.size();
    for (size_t i = 0; i < s.outputs.size(); ++i) {
        const auto* oi = s.output_info[i].get();
        if (!oi) continue;
        const std::string name = ScreenState::name_of(oi);
        if (change.display_id.empty() ? (oi->crtc != XCB_NONE) : change.display_id == name) {
            target = i;
            break;
        }
    }
    if (target == s.outputs.size() || !s.output_info[target]->crtc) {
        return Result::failure("Target display output or CRTC not found: " + change.display_id);
    }
    const auto* oi = s.output_info[target].get();
    const xcb_randr_output_t target_output = s.outputs[target];
    const xcb_randr_crtc_t crtc_id = oi->crtc;
    const auto* crtc = s.crtc(crtc_id);
    if (!crtc) return Result::failure("Failed to get CRTC info for output");

    xcb_randr_mode_t target_mode = crtc->mode;
    if (change.width.has_value() && change.height.has_value()) {
        bool found = false;
        const xcb_randr_mode_t* ids = xcb_randr_get_output_info_modes(oi);
        for (int m = 0; m < xcb_randr_get_output_info_modes_length(oi) && !found; ++m) {
            const auto* mi = find_mode(s.modes, s.nmodes, ids[m]);
            if (!mi || mi->width != *change.width || mi->height != *change.height) continue;
            if (change.refresh_rate.has_value() && std::abs(refresh_of(*mi) - *change.refresh_rate) >= 1.0) continue;
            target_mode = ids[m];
            found = true;
        }
        if (!found) return Result::failure("Requested resolution not found in output modes");
    }

    const int16_t target_x = static_cast<int16_t>(change.x.value_or(crtc->x));
    const int16_t target_y = static_cast<int16_t>(change.y.value_or(crtc->y));
    const uint16_t target_rot = change.orientation.has_value() ? orientation_to_rotation(*change.orientation)
                                                               : crtc->rotation;

    // The CRTC's new footprint, rotated, and the screen size every active
    // CRTC then needs. A CRTC may not extend past the screen, so the screen
    // grows before the CRTC change and shrinks after it.
    uint32_t mode_w = 0, mode_h = 0;
    if (const auto* mi = find_mode(s.modes, s.nmodes, target_mode)) {
        mode_w = mi->width;
        mode_h = mi->height;
    }
    if (target_rot & (XCB_RANDR_ROTATION_ROTATE_90 | XCB_RANDR_ROTATION_ROTATE_270)) std::swap(mode_w, mode_h);
    int need_w = target_x + static_cast<int>(mode_w);
    int need_h = target_y + static_cast<int>(mode_h);
    for (const auto& [id, other] : s.crtcs) {
        if (id == crtc_id || other->mode == XCB_NONE) continue;
        need_w = std::max(need_w, other->x + static_cast<int>(other->width));
        need_h = std::max(need_h, other->y + static_cast<int>(other->height));
    }
    // The real current screen size (the setup's numbers are as of connecting).
    auto geom = own(xcb_get_geometry_reply(c, xcb_get_geometry(c, root), nullptr));
    const auto* scr = conn_->screen();
    const int cur_w = geom ? geom->width : scr->width_in_pixels;
    const int cur_h = geom ? geom->height : scr->height_in_pixels;
    bool screen_error = false;
    auto resize_screen = [&](int w, int h) {
        // Keep the physical DPI: scale the mm size with the pixel size.
        uint32_t mm_w = std::max(1, scr->width_in_millimeters * w / std::max<int>(1, scr->width_in_pixels));
        uint32_t mm_h = std::max(1, scr->height_in_millimeters * h / std::max<int>(1, scr->height_in_pixels));
        auto ck = xcb_randr_set_screen_size_checked(c, root, static_cast<uint16_t>(w), static_cast<uint16_t>(h), mm_w,
                                                    mm_h);
        if (!no_error(xcb_request_check(c, ck))) screen_error = true;
    };

    // Clone outputs on the same CRTC stay attached.
    const xcb_randr_output_t* on = xcb_randr_get_crtc_info_outputs(crtc);
    std::vector<xcb_randr_output_t> outputs(on, on + xcb_randr_get_crtc_info_outputs_length(crtc));
    if (std::find(outputs.begin(), outputs.end(), target_output) == outputs.end()) outputs.push_back(target_output);

    if (need_w > cur_w || need_h > cur_h) resize_screen(std::max(need_w, cur_w), std::max(need_h, cur_h));
    xcb_generic_error_t* err = nullptr;
    auto set = own(xcb_randr_set_crtc_config_reply(
        c,
        xcb_randr_set_crtc_config(c, crtc_id, XCB_CURRENT_TIME, s.res->config_timestamp, target_x, target_y,
                                  target_mode, target_rot, static_cast<uint32_t>(outputs.size()), outputs.data()),
        &err));
    const int x_error = err ? err->error_code : 0;
    no_error(err);
    const bool set_ok = set && set->status == XCB_RANDR_SET_CONFIG_SUCCESS && x_error == 0;
    if (set_ok && (need_w != cur_w || need_h != cur_h)) resize_screen(need_w, need_h);

    // Read the CRTC back: Xwayland, for one, acknowledges the change but only
    // emulates the mode for the requesting client's fullscreen windows, so
    // the real output never changes. Report that instead of success.
    bool took_effect = false;
    if (set_ok) {
        auto after = own(xcb_randr_get_crtc_info_reply(c, xcb_randr_get_crtc_info(c, crtc_id, XCB_CURRENT_TIME), nullptr));
        took_effect = after && after->mode == target_mode && after->x == target_x && after->y == target_y &&
                      after->rotation == target_rot;
    }

    if (conn_->broken()) return Result::failure("The X server stopped answering during the mode change");
    if (x_error != 0) {
        return Result::failure("X server rejected the mode change (X error code " + std::to_string(x_error) + ")");
    }
    if (!set || set->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
        return Result::failure("RRSetCrtcConfig failed with status " + std::to_string(set ? set->status : -1));
    }
    if (screen_error) return Result::failure("X server rejected the screen size the new mode needs");
    if (!took_effect) {
        return Result::failure("X server accepted the mode change but the output did not change "
                               "(Xwayland only emulates RandR mode changes)");
    }
    return Result::success();
}

bool X11Backend::gamma_supported() {
    for (const auto& d : snapshot()) {
        if (d.night_light.supported) return true;
    }
    return false;
}

Result X11Backend::set_night_light(bool enabled, uint32_t temperature_kelvin) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_->broken()) return Result::failure("The X server stopped answering");
    xcb_connection_t* c = conn_->get();
    X11Connection::Op op(*conn_);

    if (!enabled) {
        restore_gamma_locked();
        if (conn_->broken()) return Result::failure("The X server stopped answering");
        night_light_on_ = false;
        night_light_kelvin_ = 6500;
        return Result::success();
    }

    ScreenState s;
    if (!s.load(c, conn_->root())) return Result::failure("Failed to get XRandR screen resources");
    double gr = 1, gg = 1, gb = 1;
    kelvin_to_rgb(temperature_kelvin, gr, gg, gb);

    size_t applied = 0;
    std::string why = "no active output has a gamma ramp";
    for (const auto& [crtc_id, crtc] : s.crtcs) {
        if (crtc->mode == XCB_NONE) continue;
        auto size = own(xcb_randr_get_crtc_gamma_size_reply(c, xcb_randr_get_crtc_gamma_size(c, crtc_id), nullptr));
        if (!size || size->size == 0) continue;
        auto saved = saved_gamma_.find(crtc_id);
        if (saved == saved_gamma_.end()) {
            auto g = own(xcb_randr_get_crtc_gamma_reply(c, xcb_randr_get_crtc_gamma(c, crtc_id), nullptr));
            if (!g || g->size != size->size) continue;
            SavedGamma sg;
            const uint16_t* r = xcb_randr_get_crtc_gamma_red(g.get());
            const uint16_t* gr2 = xcb_randr_get_crtc_gamma_green(g.get());
            const uint16_t* b = xcb_randr_get_crtc_gamma_blue(g.get());
            sg.red.assign(r, r + g->size);
            sg.green.assign(gr2, gr2 + g->size);
            sg.blue.assign(b, b + g->size);
            saved = saved_gamma_.emplace(crtc_id, std::move(sg)).first;
        }
        const SavedGamma& o = saved->second;
        std::vector<uint16_t> r(o.red.size()), g(o.green.size()), b(o.blue.size());
        for (size_t i = 0; i < r.size(); ++i) {
            r[i] = static_cast<uint16_t>(std::lround(o.red[i] * gr));
            g[i] = static_cast<uint16_t>(std::lround(o.green[i] * gg));
            b[i] = static_cast<uint16_t>(std::lround(o.blue[i] * gb));
        }
        auto ck = xcb_randr_set_crtc_gamma_checked(c, crtc_id, static_cast<uint16_t>(r.size()), r.data(), g.data(),
                                                   b.data());
        xcb_generic_error_t* e = xcb_request_check(c, ck);
        if (e) {
            why = "the X server rejected a gamma ramp (X error code " + std::to_string(e->error_code) + ")";
            std::free(e);
            continue;
        }
        ++applied;
    }
    if (conn_->broken()) return Result::failure("The X server stopped answering");
    if (applied == 0) return Result::failure("Night light unavailable: " + why);
    night_light_on_ = true;
    night_light_kelvin_ = temperature_kelvin;
    return Result::success();
}

void X11Backend::restore_gamma_locked() {
    if (saved_gamma_.empty()) return;
    if (!conn_->broken()) {
        xcb_connection_t* c = conn_->get();
        X11Connection::Op op(*conn_);
        for (auto& [crtc_id, sg] : saved_gamma_) {
            auto ck = xcb_randr_set_crtc_gamma_checked(c, crtc_id, static_cast<uint16_t>(sg.red.size()),
                                                       sg.red.data(), sg.green.data(), sg.blue.data());
            no_error(xcb_request_check(c, ck));
        }
    }
    saved_gamma_.clear();
}

// The watcher owns a second connection and never blocks inside XCB: it polls
// the connection fd together with a self-pipe and only takes events already
// read. stop_watcher() writes the pipe and joins.
void X11Backend::start_watcher(std::function<void()> on_change) {
    if (watching_) return;
    std::string err;
    event_conn_ = X11Connection::open(X11Connection::kIoTimeout, &err);
    if (!event_conn_) return;
    xcb_connection_t* c = event_conn_->get();
    {
        X11Connection::Op op(*event_conn_);
        const xcb_query_extension_reply_t* ext = xcb_get_extension_data(c, &xcb_randr_id);
        if (ext && ext->present) {
            rr_first_event_ = ext->first_event;
            // Checked: the roundtrip both flushes the selection and proves
            // the server took it.
            no_error(xcb_request_check(
                c, xcb_randr_select_input_checked(c, event_conn_->root(),
                                                  XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                                                      XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
                                                      XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE)));
        }
        if (!ext || !ext->present || event_conn_->broken()) {
            event_conn_.reset();
            return;
        }
    }
    if (pipe(wake_pipe_) != 0) {
        wake_pipe_[0] = wake_pipe_[1] = -1;
        event_conn_.reset();
        return;
    }
    fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);
    on_change_ = std::move(on_change);
    watching_ = true;
    watcher_thread_ = std::thread(&X11Backend::watcher_thread_func, this);
}

void X11Backend::stop_watcher() {
    if (!watching_) return;
    watching_ = false;
    char b = 1;
    (void)!write(wake_pipe_[1], &b, 1);
    if (watcher_thread_.joinable()) watcher_thread_.join();
    close(wake_pipe_[0]);
    close(wake_pipe_[1]);
    wake_pipe_[0] = wake_pipe_[1] = -1;
    event_conn_.reset();
}

void X11Backend::watcher_thread_func() {
    xcb_connection_t* c = event_conn_->get();
    const int xfd = event_conn_->fd();
    while (watching_) {
        bool changed = false;
        while (xcb_generic_event_t* ev = xcb_poll_for_event(c)) {
            const uint8_t type = ev->response_type & 0x7f;
            if (type == rr_first_event_ + XCB_RANDR_SCREEN_CHANGE_NOTIFY || type == rr_first_event_ + XCB_RANDR_NOTIFY) {
                changed = true;
            }
            std::free(ev);
        }
        if (xcb_connection_has_error(c)) break;  // the server went away
        if (changed && on_change_) on_change_();

        pollfd fds[2] = {{xfd, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
        int r = poll(fds, 2, -1);
        if (r < 0 && errno != EINTR) break;
        if (fds[1].revents & POLLIN) break;
        if (fds[0].revents & (POLLERR | POLLHUP)) break;
    }
}

} // namespace brodisplays
