#include "wayland_backend.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "wayland_internal.h"

namespace brodisplays {

namespace {

DisplayOrientation transform_to_orientation(int32_t transform) {
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_90: return DisplayOrientation::Rotate90;
        case WL_OUTPUT_TRANSFORM_180: return DisplayOrientation::Rotate180;
        case WL_OUTPUT_TRANSFORM_270: return DisplayOrientation::Rotate270;
        default: return DisplayOrientation::Normal;
    }
}

int32_t orientation_to_transform(DisplayOrientation o) {
    switch (o) {
        case DisplayOrientation::Normal: return WL_OUTPUT_TRANSFORM_NORMAL;
        case DisplayOrientation::Rotate90: return WL_OUTPUT_TRANSFORM_90;
        case DisplayOrientation::Rotate180: return WL_OUTPUT_TRANSFORM_180;
        case DisplayOrientation::Rotate270: return WL_OUTPUT_TRANSFORM_270;
    }
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

double round_hz(double hz) {
    return std::round(hz * 100.0) / 100.0;
}

void release_mode(WaylandBackend::ModeItem* m, uint32_t version) {
    if (!m->mode_obj) return;
    if (version >= ZWLR_OUTPUT_MODE_V1_RELEASE_SINCE_VERSION) {
        zwlr_output_mode_v1_release(m->mode_obj);
    } else {
        zwlr_output_mode_v1_destroy(m->mode_obj);
    }
    m->mode_obj = nullptr;
}

void release_head(WaylandBackend::HeadData* h, uint32_t version) {
    for (auto& m : h->modes) release_mode(m.get(), version);
    h->modes.clear();
    if (h->head) {
        if (version >= ZWLR_OUTPUT_HEAD_V1_RELEASE_SINCE_VERSION) {
            zwlr_output_head_v1_release(h->head);
        } else {
            zwlr_output_head_v1_destroy(h->head);
        }
        h->head = nullptr;
    }
}

void release_output(WaylandBackend::OutputData* od) {
    if (!od->output) return;
    if (od->version >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
        wl_output_release(od->output);
    } else {
        wl_output_destroy(od->output);
    }
    od->output = nullptr;
}

} // namespace

// All protocol callbacks. A nested struct so they can reach the backend's
// private state; every callback runs on the dispatching thread with
// state_mutex_ held.
struct WaylandBackend::Listeners {
    // wl_output
    static void output_geometry(void* data, wl_output*, int32_t x, int32_t y, int32_t pw, int32_t ph,
                                int32_t, const char* make, const char* model, int32_t transform) {
        auto* od = static_cast<OutputData*>(data);
        od->info.geometry.x = x;
        od->info.geometry.y = y;
        od->info.physical_width_mm = pw > 0 ? static_cast<uint32_t>(pw) : 0;
        od->info.physical_height_mm = ph > 0 ? static_cast<uint32_t>(ph) : 0;
        if (make) od->info.manufacturer = make;
        if (model) od->info.model = model;
        od->info.orientation = transform_to_orientation(transform);
    }
    static void output_mode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
        auto* od = static_cast<OutputData*>(data);
        DisplayMode m{static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                      round_hz(static_cast<double>(refresh) / 1000.0)};
        if (std::find(od->pending_modes.begin(), od->pending_modes.end(), m) == od->pending_modes.end()) {
            od->pending_modes.push_back(m);
        }
        if (flags & WL_OUTPUT_MODE_CURRENT) {
            od->info.current_mode = m;
            od->info.geometry.width = m.width;
            od->info.geometry.height = m.height;
        }
        if (od->version < WL_OUTPUT_DONE_SINCE_VERSION) {
            od->info.available_modes = od->pending_modes;
            od->done = true;
        }
    }
    static void output_done(void* data, wl_output*) {
        auto* od = static_cast<OutputData*>(data);
        od->info.available_modes = od->pending_modes;
        od->pending_modes.clear();
        od->done = true;
        if (!od->owner->wlr_manager_) od->owner->notify_change();
    }
    static void output_scale(void* data, wl_output*, int32_t factor) {
        auto* od = static_cast<OutputData*>(data);
        od->info.scale.factor = static_cast<double>(factor);
        od->info.scale.dpi = factor * 96;
    }
    static void output_name(void* data, wl_output*, const char* name) {
        auto* od = static_cast<OutputData*>(data);
        if (name) od->info.id = name;
        if (name && od->info.name.empty()) od->info.name = name;
    }
    static void output_description(void* data, wl_output*, const char* description) {
        auto* od = static_cast<OutputData*>(data);
        if (description) od->info.name = description;
    }

    // zwlr_output_mode_v1
    static void mode_size(void* data, zwlr_output_mode_v1*, int32_t w, int32_t h) {
        auto* m = static_cast<ModeItem*>(data);
        m->w = static_cast<uint32_t>(w);
        m->h = static_cast<uint32_t>(h);
    }
    static void mode_refresh(void* data, zwlr_output_mode_v1*, int32_t refresh) {
        static_cast<ModeItem*>(data)->refresh_mhz = refresh;
    }
    static void mode_preferred(void* data, zwlr_output_mode_v1*) {
        static_cast<ModeItem*>(data)->preferred = true;
    }
    static void mode_finished(void* data, zwlr_output_mode_v1*) {
        auto* m = static_cast<ModeItem*>(data);
        HeadData* h = m->head;
        if (h->current_mode_obj == m->mode_obj) h->current_mode_obj = nullptr;
        release_mode(m, h->owner->wlr_version_);
        h->modes.erase(std::remove_if(h->modes.begin(), h->modes.end(),
                                      [m](const std::unique_ptr<ModeItem>& p) { return p.get() == m; }),
                       h->modes.end());
    }

    // zwlr_output_head_v1
    static void head_name(void* data, zwlr_output_head_v1*, const char* v) {
        if (v) static_cast<HeadData*>(data)->name = v;
    }
    static void head_description(void* data, zwlr_output_head_v1*, const char* v) {
        if (v) static_cast<HeadData*>(data)->description = v;
    }
    static void head_physical_size(void* data, zwlr_output_head_v1*, int32_t w, int32_t h) {
        auto* hd = static_cast<HeadData*>(data);
        hd->phys_w = w;
        hd->phys_h = h;
    }
    static void head_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
        static const zwlr_output_mode_v1_listener kModeListener = {
            mode_size, mode_refresh, mode_preferred, mode_finished,
        };
        auto* hd = static_cast<HeadData*>(data);
        auto item = std::make_unique<ModeItem>();
        item->head = hd;
        item->mode_obj = mode;
        zwlr_output_mode_v1_add_listener(mode, &kModeListener, item.get());
        hd->modes.push_back(std::move(item));
    }
    static void head_enabled(void* data, zwlr_output_head_v1*, int32_t enabled) {
        auto* hd = static_cast<HeadData*>(data);
        hd->enabled = enabled != 0;
        if (!hd->enabled) hd->current_mode_obj = nullptr;
    }
    static void head_current_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
        static_cast<HeadData*>(data)->current_mode_obj = mode;
    }
    static void head_position(void* data, zwlr_output_head_v1*, int32_t x, int32_t y) {
        auto* hd = static_cast<HeadData*>(data);
        hd->x = x;
        hd->y = y;
    }
    static void head_transform(void* data, zwlr_output_head_v1*, int32_t t) {
        static_cast<HeadData*>(data)->transform = t;
    }
    static void head_scale(void* data, zwlr_output_head_v1*, wl_fixed_t s) {
        static_cast<HeadData*>(data)->scale = wl_fixed_to_double(s);
    }
    static void head_finished(void* data, zwlr_output_head_v1*) {
        auto* hd = static_cast<HeadData*>(data);
        WaylandBackend* self = hd->owner;
        release_head(hd, self->wlr_version_);
        self->heads_.erase(std::remove(self->heads_.begin(), self->heads_.end(), hd), self->heads_.end());
        delete hd;
    }
    static void head_make(void* data, zwlr_output_head_v1*, const char* v) {
        if (v) static_cast<HeadData*>(data)->make = v;
    }
    static void head_model(void* data, zwlr_output_head_v1*, const char* v) {
        if (v) static_cast<HeadData*>(data)->model = v;
    }
    static void head_serial(void* data, zwlr_output_head_v1*, const char* v) {
        if (v) static_cast<HeadData*>(data)->serial_number = v;
    }
    static void head_adaptive_sync(void*, zwlr_output_head_v1*, uint32_t) {}

    // zwlr_output_manager_v1
    static void manager_head(void* data, zwlr_output_manager_v1*, zwlr_output_head_v1* head) {
        static const zwlr_output_head_v1_listener kHeadListener = {
            head_name, head_description, head_physical_size, head_mode, head_enabled,
            head_current_mode, head_position, head_transform, head_scale, head_finished,
            head_make, head_model, head_serial, head_adaptive_sync,
        };
        auto* self = static_cast<WaylandBackend*>(data);
        auto* hd = new HeadData();
        hd->owner = self;
        hd->head = head;
        zwlr_output_head_v1_add_listener(head, &kHeadListener, hd);
        self->heads_.push_back(hd);
    }
    static void manager_done(void* data, zwlr_output_manager_v1*, uint32_t serial) {
        auto* self = static_cast<WaylandBackend*>(data);
        self->wlr_serial_ = serial;
        ++self->wlr_done_count_;
        self->notify_change();
    }
    static void manager_finished(void* data, zwlr_output_manager_v1* mgr) {
        auto* self = static_cast<WaylandBackend*>(data);
        zwlr_output_manager_v1_destroy(mgr);
        if (self->wlr_manager_ == mgr) self->wlr_manager_ = nullptr;
    }

    // wl_registry
    static void global(void* data, wl_registry* reg, uint32_t id, const char* interface, uint32_t version) {
        static const wl_output_listener kOutputListener = {
            output_geometry, output_mode, output_done, output_scale, output_name, output_description,
        };
        static const zwlr_output_manager_v1_listener kManagerListener = {
            manager_head, manager_done, manager_finished,
        };
        auto* self = static_cast<WaylandBackend*>(data);
        if (std::strcmp(interface, wl_output_interface.name) == 0) {
            auto* od = new OutputData();
            od->owner = self;
            od->id = id;
            od->version = std::min(version, 4u);
            od->output = static_cast<wl_output*>(wl_registry_bind(reg, id, &wl_output_interface, od->version));
            wl_output_add_listener(od->output, &kOutputListener, od);
            self->outputs_.push_back(od);
        } else if (std::strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0 &&
                   !self->gamma_manager_) {
            self->gamma_manager_ = static_cast<zwlr_gamma_control_manager_v1*>(
                wl_registry_bind(reg, id, &zwlr_gamma_control_manager_v1_interface, 1));
        } else if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) == 0 && !self->wlr_manager_) {
            self->wlr_version_ = std::min(version, 4u);
            self->wlr_manager_ = static_cast<zwlr_output_manager_v1*>(
                wl_registry_bind(reg, id, &zwlr_output_manager_v1_interface, self->wlr_version_));
            zwlr_output_manager_v1_add_listener(self->wlr_manager_, &kManagerListener, self);
        }
    }
    static void global_remove(void* data, wl_registry*, uint32_t id) {
        auto* self = static_cast<WaylandBackend*>(data);
        for (auto it = self->outputs_.begin(); it != self->outputs_.end(); ++it) {
            if ((*it)->id == id) {
                self->drop_gamma(*it);
                release_output(*it);
                delete *it;
                self->outputs_.erase(it);
                if (!self->wlr_manager_) self->notify_change();
                break;
            }
        }
    }

    // wl_callback (sync)
    static void sync_done(void* data, wl_callback* cb, uint32_t) {
        *static_cast<bool*>(data) = true;
        wl_callback_destroy(cb);
    }
};

std::unique_ptr<WaylandBackend> WaylandBackend::create(std::string* error) {
    // Only a session that names a compositor is a Wayland session; libwayland
    // would otherwise fall back to "wayland-0" and pick up an unrelated one.
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    const char* ws = std::getenv("WAYLAND_SOCKET");
    if ((!wd || !*wd) && (!ws || !*ws)) {
        if (error) *error = "WAYLAND_DISPLAY is not set";
        return nullptr;
    }
    wl_display* d = wl_display_connect(nullptr);
    if (!d) {
        if (error) *error = "Failed to connect to Wayland display";
        return nullptr;
    }
    std::unique_ptr<WaylandBackend> backend(new WaylandBackend(d));
    if (!backend->initialize(error)) return nullptr;
    return backend;
}

WaylandBackend::WaylandBackend(wl_display* display)
    : display_(display) {}

bool WaylandBackend::initialize(std::string* error) {
    static const wl_registry_listener kRegistryListener = {Listeners::global, Listeners::global_remove};
    std::unique_lock<std::recursive_mutex> lock(state_mutex_);
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    // First roundtrip delivers the globals; the second the initial output and
    // head state the binds triggered.
    if (!roundtrip(lock) || !roundtrip(lock)) {
        if (error) *error = "Wayland compositor did not answer the initial registry roundtrip";
        return false;
    }
    return true;
}

WaylandBackend::~WaylandBackend() {
    stop_watcher();

    for (auto* h : heads_) {
        release_head(h, wlr_version_);
        delete h;
    }
    heads_.clear();

    for (auto* od : outputs_) {
        drop_gamma(od);
        release_output(od);
        delete od;
    }
    outputs_.clear();

    if (gamma_manager_) {
        zwlr_gamma_control_manager_v1_destroy(gamma_manager_);
        gamma_manager_ = nullptr;
    }
    if (wlr_manager_) {
        zwlr_output_manager_v1_destroy(wlr_manager_);
        wlr_manager_ = nullptr;
    }
    if (registry_) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

void WaylandBackend::mark_broken(const char* why) {
    if (!broken_.exchange(true)) {
        std::fprintf(stderr, "brodisplays: Wayland backend disabled: %s\n", why);
    }
}

void WaylandBackend::notify_change() {
    if (on_change_) on_change_();
}

bool WaylandBackend::dispatch_until(std::unique_lock<std::recursive_mutex>& lock,
                                    const std::function<bool()>& done,
                                    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (broken_) return done();

    if (running_) {
        // The watcher thread reads and dispatches; it needs the lock to do so.
        wl_display_flush(display_);
        while (!done()) {
            if (broken_ || std::chrono::steady_clock::now() >= deadline) return done();
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            lock.lock();
        }
        return true;
    }

    const int fd = wl_display_get_fd(display_);
    while (!done()) {
        while (wl_display_prepare_read(display_) != 0) {
            if (wl_display_dispatch_pending(display_) < 0) {
                mark_broken("protocol error while dispatching");
                return done();
            }
            if (done()) return true;
        }
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display_);
            mark_broken("connection lost while flushing");
            return done();
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            wl_display_cancel_read(display_);
            return done();
        }
        pollfd p{fd, POLLIN, 0};
        int r = poll(&p, 1, static_cast<int>(remaining.count()));
        if (r <= 0) {
            wl_display_cancel_read(display_);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) mark_broken("poll on the Wayland socket failed");
            return done();
        }
        if (wl_display_read_events(display_) < 0) {
            mark_broken("reading Wayland events failed");
            return done();
        }
        if (wl_display_dispatch_pending(display_) < 0) {
            mark_broken("protocol error while dispatching");
            return done();
        }
    }
    return true;
}

bool WaylandBackend::roundtrip(std::unique_lock<std::recursive_mutex>& lock) {
    static const wl_callback_listener kSyncListener = {Listeners::sync_done};
    if (broken_) return false;
    // Heap flag: if the compositor answers after we gave up, the callback
    // still writes somewhere valid (the proxy is destroyed on timeout, so in
    // practice it is never dispatched).
    auto flag = std::make_unique<bool>(false);
    wl_callback* cb = wl_display_sync(display_);
    wl_callback_add_listener(cb, &kSyncListener, flag.get());
    bool* f = flag.get();
    if (!dispatch_until(lock, [f] { return *f; }, kIoTimeout)) {
        if (!*f) wl_callback_destroy(cb);
        mark_broken("compositor did not answer a sync request within the I/O timeout");
        return false;
    }
    return true;
}

bool WaylandBackend::can_configure() {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    return wlr_manager_ != nullptr && !broken_;
}

std::vector<DisplayInfo> WaylandBackend::snapshot() {
    std::unique_lock<std::recursive_mutex> lock(state_mutex_);
    std::vector<DisplayInfo> results;
    if (broken_) return results;
    if (!running_ && !roundtrip(lock)) return results;
    if (!probe_gamma(lock)) return results;

    // Night light as this backend shows it: through its gamma controls.
    auto night_light_of = [this](const std::string& output_name) {
        NightLightStatus st;
        for (const auto* od : outputs_) {
            if (od->info.id != output_name) continue;
            const bool held = od->gamma && !od->gamma->failed;
            st.supported = gamma_manager_ != nullptr && (held || od->gamma_usable == 1);
            if (held && night_light_on_) {
                st.enabled = true;
                st.temperature_kelvin = night_light_kelvin_;
            }
        }
        return st;
    };

    if (wlr_manager_ && wlr_done_count_ > 0) {
        for (const auto* h : heads_) {
            if (!h->enabled) continue;

            DisplayInfo info;
            info.id = h->name.empty() ? ("Head-" + std::to_string(results.size() + 1)) : h->name;
            info.device_name = h->name;
            info.name = h->description.empty() ? info.id : h->description;
            info.manufacturer = h->make;
            info.model = h->model.empty() ? info.name : h->model;
            info.adapter_name = "Wayland Compositor Output";
            info.is_connected = true;
            info.is_primary = results.empty();
            info.physical_width_mm = h->phys_w > 0 ? static_cast<uint32_t>(h->phys_w) : 0;
            info.physical_height_mm = h->phys_h > 0 ? static_cast<uint32_t>(h->phys_h) : 0;
            info.geometry.x = h->x;
            info.geometry.y = h->y;
            info.orientation = transform_to_orientation(h->transform);
            info.scale.factor = h->scale > 0.0 ? h->scale : 1.0;
            info.scale.dpi = static_cast<int32_t>(std::round(info.scale.factor * 96.0));

            for (const auto& m : h->modes) {
                // A mode without a refresh event has no fixed rate: report 0.
                DisplayMode dm{m->w, m->h, round_hz(static_cast<double>(m->refresh_mhz) / 1000.0)};
                if (std::find(info.available_modes.begin(), info.available_modes.end(), dm) ==
                    info.available_modes.end()) {
                    info.available_modes.push_back(dm);
                }
                if (m->mode_obj && m->mode_obj == h->current_mode_obj) {
                    info.current_mode = dm;
                }
            }
            // Logical size: the mode rotated and divided by the scale.
            uint32_t w = info.current_mode.width, hgt = info.current_mode.height;
            if (h->transform % 2 == 1) std::swap(w, hgt);
            info.geometry.width = static_cast<uint32_t>(std::lround(w / info.scale.factor));
            info.geometry.height = static_cast<uint32_t>(std::lround(hgt / info.scale.factor));
            info.night_light = night_light_of(h->name);

            results.push_back(std::move(info));
        }
        return results;
    }

    for (const auto* od : outputs_) {
        if (!od->done) continue;
        DisplayInfo di = od->info;
        if (di.id.empty()) {
            di.id = "wl_output-" + std::to_string(od->id);
            if (di.name.empty()) di.name = di.id;
        }
        di.adapter_name = "Wayland Compositor Output";
        di.device_name = di.id;
        di.night_light = night_light_of(od->info.id);
        di.is_connected = true;
        di.is_primary = results.empty();
        results.push_back(std::move(di));
    }
    return results;
}

Result WaylandBackend::apply_configuration(const DisplayConfigChange& change) {
    std::unique_lock<std::recursive_mutex> lock(state_mutex_);
    if (broken_) {
        return Result::failure("Wayland compositor connection is not usable");
    }
    if (!wlr_manager_) {
        return Result::failure("wlr-output-management protocol not supported by this Wayland compositor");
    }

    HeadData* target = nullptr;
    for (auto* h : heads_) {
        if (change.display_id.empty() ? h->enabled : h->name == change.display_id) {
            target = h;
            break;
        }
    }
    if (!target) {
        return Result::failure("Output head not found: " + change.display_id);
    }

    zwlr_output_mode_v1* target_mode = nullptr;
    bool custom_mode = false;
    if (change.width.has_value() && change.height.has_value()) {
        for (const auto& m : target->modes) {
            if (m->w != *change.width || m->h != *change.height) continue;
            if (change.refresh_rate.has_value() &&
                std::abs(static_cast<double>(m->refresh_mhz) / 1000.0 - *change.refresh_rate) >= 1.0) {
                continue;
            }
            target_mode = m->mode_obj;
            break;
        }
        custom_mode = target_mode == nullptr;
    }

    // The protocol requires every head to be either enabled or disabled in a
    // configuration; untouched heads are re-stated with their current state.
    auto* config = zwlr_output_manager_v1_create_configuration(wlr_manager_, wlr_serial_);
    std::vector<zwlr_output_configuration_head_v1*> config_heads;
    for (auto* h : heads_) {
        if (h != target && !h->enabled) {
            zwlr_output_configuration_v1_disable_head(config, h->head);
            continue;
        }
        auto* ch = zwlr_output_configuration_v1_enable_head(config, h->head);
        config_heads.push_back(ch);
        if (h == target && target_mode) {
            zwlr_output_configuration_head_v1_set_mode(ch, target_mode);
        } else if (h == target && custom_mode) {
            int32_t mhz = change.refresh_rate.has_value()
                ? static_cast<int32_t>(std::lround(*change.refresh_rate * 1000.0)) : 0;
            zwlr_output_configuration_head_v1_set_custom_mode(
                ch, static_cast<int32_t>(*change.width), static_cast<int32_t>(*change.height), mhz);
        } else if (h->current_mode_obj) {
            zwlr_output_configuration_head_v1_set_mode(ch, h->current_mode_obj);
        }
        int32_t x = h->x, y = h->y;
        if (h == target && change.x.has_value()) x = *change.x;
        if (h == target && change.y.has_value()) y = *change.y;
        zwlr_output_configuration_head_v1_set_position(ch, x, y);
        int32_t transform = h->transform;
        if (h == target && change.orientation.has_value()) transform = orientation_to_transform(*change.orientation);
        zwlr_output_configuration_head_v1_set_transform(ch, transform);
        double scale = h->scale;
        if (h == target && change.scale_factor.has_value()) scale = *change.scale_factor;
        zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(scale));
    }

    struct Outcome {
        bool finished = false;
        const char* what = "";
    };
    auto outcome = std::make_unique<Outcome>();
    static const zwlr_output_configuration_v1_listener kConfigListener = {
        [](void* data, zwlr_output_configuration_v1*) {
            auto* o = static_cast<Outcome*>(data);
            o->finished = true;
            o->what = "succeeded";
        },
        [](void* data, zwlr_output_configuration_v1*) {
            auto* o = static_cast<Outcome*>(data);
            o->finished = true;
            o->what = "failed";
        },
        [](void* data, zwlr_output_configuration_v1*) {
            auto* o = static_cast<Outcome*>(data);
            o->finished = true;
            o->what = "cancelled";
        },
    };
    zwlr_output_configuration_v1_add_listener(config, &kConfigListener, outcome.get());

    zwlr_output_configuration_v1_apply(config);
    Outcome* o = outcome.get();
    dispatch_until(lock, [o] { return o->finished; }, kIoTimeout);

    for (auto* ch : config_heads) zwlr_output_configuration_head_v1_destroy(ch);
    zwlr_output_configuration_v1_destroy(config);

    if (!o->finished) {
        return Result::failure("Wayland compositor did not answer the output configuration request");
    }
    if (std::strcmp(o->what, "succeeded") != 0) {
        return Result::failure(std::string("Compositor ") + o->what + " the output configuration" +
                               (std::strcmp(o->what, "cancelled") == 0 ? " (outputs changed meanwhile)" : ""));
    }

    // The compositor queues the new head state (ending in a manager `done`)
    // before it answers anything sent after `succeeded`, so one roundtrip
    // makes a snapshot right after this call reflect the change. (A no-op
    // configuration produces no `done` at all, so waiting for one would stall.)
    roundtrip(lock);
    return Result::success();
}

void WaylandBackend::start_watcher(std::function<void()> on_change) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (running_ || broken_) return;
    if (pipe(wake_pipe_) != 0) {
        wake_pipe_[0] = -1;
        wake_pipe_[1] = -1;
        return; // without a wake pipe the thread could not be stopped
    }
    fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);
    on_change_ = std::move(on_change);
    running_ = true;
    loop_thread_ = std::thread(&WaylandBackend::event_loop_func, this);
}

void WaylandBackend::stop_watcher() {
    if (!running_) return;
    running_ = false;
    char b = 1;
    (void)!write(wake_pipe_[1], &b, 1);
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    close(wake_pipe_[0]);
    close(wake_pipe_[1]);
    wake_pipe_[0] = -1;
    wake_pipe_[1] = -1;
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    on_change_ = nullptr;
}

void WaylandBackend::event_loop_func() {
    while (running_ && !broken_) {
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            while (wl_display_prepare_read(display_) != 0) {
                if (wl_display_dispatch_pending(display_) < 0) {
                    mark_broken("protocol error while dispatching");
                    return;
                }
            }
            wl_display_flush(display_);
        }

        pollfd fds[2];
        fds[0] = pollfd{wl_display_get_fd(display_), POLLIN, 0};
        fds[1] = pollfd{wake_pipe_[0], POLLIN, 0};
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            wl_display_cancel_read(display_);
            if (errno == EINTR) continue;
            mark_broken("poll on the Wayland socket failed");
            break;
        }
        if (!running_ || (fds[1].revents & POLLIN)) {
            wl_display_cancel_read(display_);
            break;
        }
        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(display_) < 0) {
                mark_broken("reading Wayland events failed");
                break;
            }
        } else {
            wl_display_cancel_read(display_);
        }

        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        if (wl_display_dispatch_pending(display_) < 0) {
            mark_broken("protocol error while dispatching");
            break;
        }
    }
}

} // namespace brodisplays
