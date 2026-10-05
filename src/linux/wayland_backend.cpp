#include "wayland_backend.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <wayland-client.h>
#include "wlr-output-management-unstable-v1-client-protocol.h"

namespace brodisplays {

struct WaylandBackend::OutputData {
    uint32_t id = 0;
    wl_output* output = nullptr;
    DisplayInfo info;
    bool done = false;
};

struct WaylandBackend::HeadData {
    zwlr_output_head_v1* head = nullptr;
    std::string name;
    std::string description;
    std::string make;
    std::string model;
    std::string serial_number;
    int32_t phys_w = 0;
    int32_t phys_h = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t transform = 0;
    double scale = 1.0;
    bool enabled = false;

    struct ModeItem {
        zwlr_output_mode_v1* mode_obj = nullptr;
        uint32_t w = 0;
        uint32_t h = 0;
        int32_t refresh_mhz = 0;
        bool preferred = false;
    };
    std::vector<ModeItem> modes;
    zwlr_output_mode_v1* current_mode_obj = nullptr;
};

namespace {

// wl_output listener
void output_handle_geometry(
    void* data, struct wl_output* /*output*/,
    int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height,
    int32_t /*subpixel*/,
    const char* make, const char* model,
    int32_t transform) {
    auto* od = static_cast<WaylandBackend::OutputData*>(data);
    od->info.geometry.x = x;
    od->info.geometry.y = y;
    od->info.physical_width_mm = physical_width > 0 ? physical_width : 0;
    od->info.physical_height_mm = physical_height > 0 ? physical_height : 0;
    if (make) od->info.manufacturer = make;
    if (model) od->info.model = model;

    switch (transform) {
        case WL_OUTPUT_TRANSFORM_90: od->info.orientation = DisplayOrientation::Rotate90; break;
        case WL_OUTPUT_TRANSFORM_180: od->info.orientation = DisplayOrientation::Rotate180; break;
        case WL_OUTPUT_TRANSFORM_270: od->info.orientation = DisplayOrientation::Rotate270; break;
        default: od->info.orientation = DisplayOrientation::Normal; break;
    }
}

void output_handle_mode(
    void* data, struct wl_output* /*output*/,
    uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    auto* od = static_cast<WaylandBackend::OutputData*>(data);
    double rr = static_cast<double>(refresh) / 1000.0;
    DisplayMode m{static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::round(rr * 100.0) / 100.0};

    od->info.available_modes.push_back(m);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        od->info.current_mode = m;
        od->info.geometry.width = m.width;
        od->info.geometry.height = m.height;
    }
}

void output_handle_done(void* data, struct wl_output* /*output*/) {
    auto* od = static_cast<WaylandBackend::OutputData*>(data);
    od->done = true;
}

void output_handle_scale(void* data, struct wl_output* /*output*/, int32_t factor) {
    auto* od = static_cast<WaylandBackend::OutputData*>(data);
    od->info.scale.factor = static_cast<double>(factor);
    od->info.scale.dpi = factor * 96;
}

void output_handle_name(void* data, struct wl_output* /*output*/, const char* name) {
    auto* od = static_cast<WaylandBackend::OutputData*>(data);
    if (name) {
        od->info.id = name;
        od->info.name = name;
    }
}

void output_handle_description(void* data, struct wl_output* /*output*/, const char* description) {
    auto* od = static_cast<WaylandBackend::OutputData*>(data);
    if (description) {
        od->info.name = description;
    }
}

const struct wl_output_listener kOutputListener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale,
    output_handle_name,
    output_handle_description,
};

// zwlr_output_mode_v1 listeners
void wlr_mode_size(void* data, struct zwlr_output_mode_v1* /*mode*/, int32_t width, int32_t height) {
    auto* m = static_cast<WaylandBackend::HeadData::ModeItem*>(data);
    m->w = static_cast<uint32_t>(width);
    m->h = static_cast<uint32_t>(height);
}

void wlr_mode_refresh(void* data, struct zwlr_output_mode_v1* /*mode*/, int32_t refresh) {
    auto* m = static_cast<WaylandBackend::HeadData::ModeItem*>(data);
    m->refresh_mhz = refresh;
}

void wlr_mode_preferred(void* data, struct zwlr_output_mode_v1* /*mode*/) {
    auto* m = static_cast<WaylandBackend::HeadData::ModeItem*>(data);
    m->preferred = true;
}

void wlr_mode_finished(void* /*data*/, struct zwlr_output_mode_v1* mode) {
    zwlr_output_mode_v1_destroy(mode);
}

const struct zwlr_output_mode_v1_listener kWlrModeListener = {
    wlr_mode_size,
    wlr_mode_refresh,
    wlr_mode_preferred,
    wlr_mode_finished,
};

// zwlr_output_head_v1 listeners
void wlr_head_name(void* data, struct zwlr_output_head_v1* /*head*/, const char* name) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    if (name) h->name = name;
}

void wlr_head_description(void* data, struct zwlr_output_head_v1* /*head*/, const char* description) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    if (description) h->description = description;
}

void wlr_head_physical_size(void* data, struct zwlr_output_head_v1* /*head*/, int32_t width, int32_t height) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    h->phys_w = width;
    h->phys_h = height;
}

void wlr_head_mode(void* data, struct zwlr_output_head_v1* /*head*/, struct zwlr_output_mode_v1* mode) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    h->modes.push_back(WaylandBackend::HeadData::ModeItem{mode, 0, 0, 0, false});
    zwlr_output_mode_v1_add_listener(mode, &kWlrModeListener, &h->modes.back());
}

void wlr_head_enabled(void* data, struct zwlr_output_head_v1* /*head*/, int32_t enabled) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    h->enabled = (enabled != 0);
}

void wlr_head_current_mode(void* data, struct zwlr_output_head_v1* /*head*/, struct zwlr_output_mode_v1* mode) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    h->current_mode_obj = mode;
}

void wlr_head_position(void* data, struct zwlr_output_head_v1* /*head*/, int32_t x, int32_t y) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    h->x = x;
    h->y = y;
}

void wlr_head_transform(void* data, struct zwlr_output_head_v1* /*head*/, int32_t transform) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    h->transform = transform;
}

void wlr_head_scale(void* data, struct zwlr_output_head_v1* /*head*/, wl_fixed_t scale) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    h->scale = wl_fixed_to_double(scale);
}

void wlr_head_finished(void* /*data*/, struct zwlr_output_head_v1* head) {
    zwlr_output_head_v1_destroy(head);
}

void wlr_head_make(void* data, struct zwlr_output_head_v1* /*head*/, const char* make) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    if (make) h->make = make;
}

void wlr_head_model(void* data, struct zwlr_output_head_v1* /*head*/, const char* model) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    if (model) h->model = model;
}

void wlr_head_serial(void* data, struct zwlr_output_head_v1* /*head*/, const char* serial) {
    auto* h = static_cast<WaylandBackend::HeadData*>(data);
    if (serial) h->serial_number = serial;
}

void wlr_head_adaptive_sync(void* /*data*/, struct zwlr_output_head_v1* /*head*/, uint32_t /*state*/) {}

const struct zwlr_output_head_v1_listener kWlrHeadListener = {
    wlr_head_name,
    wlr_head_description,
    wlr_head_physical_size,
    wlr_head_mode,
    wlr_head_enabled,
    wlr_head_current_mode,
    wlr_head_position,
    wlr_head_transform,
    wlr_head_scale,
    wlr_head_finished,
    wlr_head_make,
    wlr_head_model,
    wlr_head_serial,
    wlr_head_adaptive_sync,
};

} // namespace

bool WaylandBackend::is_available() {
    wl_display* d = wl_display_connect(nullptr);
    if (!d) return false;
    wl_display_disconnect(d);
    return true;
}

std::unique_ptr<WaylandBackend> WaylandBackend::create(std::string* error) {
    wl_display* d = wl_display_connect(nullptr);
    if (!d) {
        if (error) *error = "Failed to connect to Wayland display";
        return nullptr;
    }
    return std::unique_ptr<WaylandBackend>(new WaylandBackend(d));
}

WaylandBackend::WaylandBackend(wl_display* display)
    : display_(display) {
    registry_ = wl_display_get_registry(display_);

    static const struct wl_registry_listener registry_listener = {
        [](void* data, struct wl_registry* reg, uint32_t id, const char* interface, uint32_t version) {
            auto* self = static_cast<WaylandBackend*>(data);
            if (std::strcmp(interface, wl_output_interface.name) == 0) {
                uint32_t ver = std::min(version, 4u);
                auto* out = static_cast<wl_output*>(wl_registry_bind(reg, id, &wl_output_interface, ver));
                auto* od = new OutputData();
                od->id = id;
                od->output = out;
                wl_output_add_listener(out, &kOutputListener, od);
                self->outputs_.push_back(od);
            } else if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
                uint32_t ver = std::min(version, 4u);
                self->wlr_manager_ = static_cast<zwlr_output_manager_v1*>(
                    wl_registry_bind(reg, id, &zwlr_output_manager_v1_interface, ver));

                static const struct zwlr_output_manager_v1_listener manager_listener = {
                    [](void* mdata, struct zwlr_output_manager_v1* /*mgr*/, struct zwlr_output_head_v1* head) {
                        auto* s = static_cast<WaylandBackend*>(mdata);
                        auto* hd = new HeadData();
                        hd->head = head;
                        zwlr_output_head_v1_add_listener(head, &kWlrHeadListener, hd);
                        s->heads_.push_back(hd);
                    },
                    [](void* mdata, struct zwlr_output_manager_v1* /*mgr*/, uint32_t serial) {
                        auto* s = static_cast<WaylandBackend*>(mdata);
                        s->wlr_serial_ = serial;
                        if (s->on_change_) {
                            s->on_change_();
                        }
                    },
                    [](void* /*mdata*/, struct zwlr_output_manager_v1* mgr) {
                        zwlr_output_manager_v1_destroy(mgr);
                    }
                };
                zwlr_output_manager_v1_add_listener(self->wlr_manager_, &manager_listener, self);
            }
        },
        [](void* data, struct wl_registry* /*reg*/, uint32_t id) {
            auto* self = static_cast<WaylandBackend*>(data);
            for (auto it = self->outputs_.begin(); it != self->outputs_.end(); ++it) {
                if ((*it)->id == id) {
                    wl_output_destroy((*it)->output);
                    delete *it;
                    self->outputs_.erase(it);
                    break;
                }
            }
        }
    };

    wl_registry_add_listener(registry_, &registry_listener, this);
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);
}

WaylandBackend::~WaylandBackend() {
    stop_watcher();

    for (auto* h : heads_) {
        zwlr_output_head_v1_destroy(h->head);
        delete h;
    }
    heads_.clear();

    for (auto* od : outputs_) {
        wl_output_destroy(od->output);
        delete od;
    }
    outputs_.clear();

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

std::vector<DisplayInfo> WaylandBackend::snapshot() {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (display_ && !running_) {
        wl_display_roundtrip(display_);
    }

    std::vector<DisplayInfo> results;

    // Prefer wlr-output-management heads if available
    if (!heads_.empty()) {
        for (const auto* h : heads_) {
            if (!h->enabled) continue;

            DisplayInfo info;
            info.id = h->name.empty() ? ("Head-" + std::to_string(results.size() + 1)) : h->name;
            info.name = h->description.empty() ? info.id : h->description;
            info.manufacturer = h->make.empty() ? "Wayland" : h->make;
            info.model = h->model.empty() ? info.name : h->model;
            info.adapter_name = "Wayland Compositor Output";
            info.is_connected = true;
            info.is_primary = results.empty();
            info.physical_width_mm = h->phys_w > 0 ? static_cast<uint32_t>(h->phys_w) : 0;
            info.physical_height_mm = h->phys_h > 0 ? static_cast<uint32_t>(h->phys_h) : 0;

            info.geometry.x = h->x;
            info.geometry.y = h->y;

            switch (h->transform) {
                case WL_OUTPUT_TRANSFORM_90: info.orientation = DisplayOrientation::Rotate90; break;
                case WL_OUTPUT_TRANSFORM_180: info.orientation = DisplayOrientation::Rotate180; break;
                case WL_OUTPUT_TRANSFORM_270: info.orientation = DisplayOrientation::Rotate270; break;
                default: info.orientation = DisplayOrientation::Normal; break;
            }

            info.scale.factor = h->scale > 0.0 ? h->scale : 1.0;
            info.scale.dpi = static_cast<int32_t>(std::round(info.scale.factor * 96.0));

            for (const auto& m : h->modes) {
                double rr = m.refresh_mhz > 0 ? static_cast<double>(m.refresh_mhz) / 1000.0 : 60.0;
                DisplayMode dm{m.w, m.h, std::round(rr * 100.0) / 100.0};
                info.available_modes.push_back(dm);

                if (m.mode_obj == h->current_mode_obj) {
                    info.current_mode = dm;
                    info.geometry.width = dm.width;
                    info.geometry.height = dm.height;
                }
            }

            if (info.current_mode.width == 0 && !info.available_modes.empty()) {
                info.current_mode = info.available_modes.front();
                info.geometry.width = info.current_mode.width;
                info.geometry.height = info.current_mode.height;
            }

            results.push_back(info);
        }
    } else {
        // Fall back to wl_output
        for (const auto* od : outputs_) {
            DisplayInfo di = od->info;
            if (di.id.empty()) {
                di.id = "wl_output-" + std::to_string(od->id);
                di.name = di.id;
            }
            di.adapter_name = "Wayland Compositor Output";
            di.is_connected = true;
            di.is_primary = results.empty();
            results.push_back(di);
        }
    }

    return results;
}

Result WaylandBackend::apply_configuration(const DisplayConfigChange& change) {
    std::unique_lock<std::recursive_mutex> lock(state_mutex_);
    if (!wlr_manager_) {
        return Result::failure("wlr-output-management protocol not supported by this Wayland compositor");
    }

    HeadData* target_head = nullptr;
    for (auto* h : heads_) {
        if (change.display_id.empty() || h->name == change.display_id) {
            target_head = h;
            break;
        }
    }

    if (!target_head) {
        return Result::failure("Output head not found: " + change.display_id);
    }

    // Find requested mode
    zwlr_output_mode_v1* target_mode = nullptr;
    if (change.width.has_value() && change.height.has_value()) {
        for (const auto& m : target_head->modes) {
            if (m.w == *change.width && m.h == *change.height) {
                if (!change.refresh_rate.has_value() ||
                    std::abs((static_cast<double>(m.refresh_mhz) / 1000.0) - *change.refresh_rate) < 1.0) {
                    target_mode = m.mode_obj;
                    break;
                }
            }
        }
    }

    zwlr_output_configuration_v1* config = zwlr_output_manager_v1_create_configuration(
        wlr_manager_, wlr_serial_);
    if (!config) {
        return Result::failure("Failed to create zwlr_output_configuration_v1");
    }

    zwlr_output_configuration_head_v1* config_head =
        zwlr_output_configuration_v1_enable_head(config, target_head->head);

    if (target_mode) {
        zwlr_output_configuration_head_v1_set_mode(config_head, target_mode);
    } else if (change.width.has_value() && change.height.has_value()) {
        int32_t mhz = change.refresh_rate.has_value() ? static_cast<int32_t>(*change.refresh_rate * 1000) : 0;
        zwlr_output_configuration_head_v1_set_custom_mode(
            config_head, *change.width, *change.height, mhz);
    }

    if (change.x.has_value() && change.y.has_value()) {
        zwlr_output_configuration_head_v1_set_position(config_head, *change.x, *change.y);
    }

    if (change.scale_factor.has_value()) {
        zwlr_output_configuration_head_v1_set_scale(
            config_head, wl_fixed_from_double(*change.scale_factor));
    }

    // Apply configuration
    struct ConfigResult {
        bool finished = false;
        bool success = false;
    } result;

    static const struct zwlr_output_configuration_v1_listener config_listener = {
        [](void* data, struct zwlr_output_configuration_v1* cfg) {
            auto* r = static_cast<ConfigResult*>(data);
            r->finished = true;
            r->success = true;
            zwlr_output_configuration_v1_destroy(cfg);
        },
        [](void* data, struct zwlr_output_configuration_v1* cfg) {
            auto* r = static_cast<ConfigResult*>(data);
            r->finished = true;
            r->success = false;
            zwlr_output_configuration_v1_destroy(cfg);
        },
        [](void* data, struct zwlr_output_configuration_v1* cfg) {
            auto* r = static_cast<ConfigResult*>(data);
            r->finished = true;
            r->success = false;
            zwlr_output_configuration_v1_destroy(cfg);
        }
    };

    zwlr_output_configuration_v1_add_listener(config, &config_listener, &result);
    zwlr_output_configuration_v1_apply(config);
    wl_display_flush(display_);

    lock.unlock();

    auto start_time = std::chrono::steady_clock::now();
    while (!result.finished) {
        if (!running_) {
            if (wl_display_dispatch(display_) < 0) {
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(2)) {
            break;
        }
    }

    if (!result.success) {
        return Result::failure("Compositor rejected output configuration");
    }

    return Result::success();
}

void WaylandBackend::start_watcher(std::function<void()> on_change) {
    if (running_) return;
    if (pipe(wake_pipe_) != 0) {
        wake_pipe_[0] = -1;
        wake_pipe_[1] = -1;
    } else {
        fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);
    }
    on_change_ = std::move(on_change);
    running_ = true;
    loop_thread_ = std::thread(&WaylandBackend::event_loop_func, this);
}

void WaylandBackend::stop_watcher() {
    if (!running_) return;
    running_ = false;
    if (wake_pipe_[1] != -1) {
        char b = 1;
        (void)write(wake_pipe_[1], &b, 1);
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    if (wake_pipe_[0] != -1) {
        close(wake_pipe_[0]);
        close(wake_pipe_[1]);
        wake_pipe_[0] = -1;
        wake_pipe_[1] = -1;
    }
}

void WaylandBackend::event_loop_func() {
    while (running_) {
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            while (wl_display_prepare_read(display_) != 0) {
                wl_display_dispatch_pending(display_);
            }
            wl_display_flush(display_);
        }

        pollfd fds[2];
        fds[0].fd = wl_display_get_fd(display_);
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = wake_pipe_[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int ret = poll(fds, wake_pipe_[0] != -1 ? 2 : 1, -1);
        if (ret < 0) {
            wl_display_cancel_read(display_);
            if (errno == EINTR) continue;
            break;
        }
        if (!running_) {
            wl_display_cancel_read(display_);
            break;
        }

        if (wake_pipe_[0] != -1 && (fds[1].revents & POLLIN)) {
            char buf[16];
            (void)read(wake_pipe_[0], buf, sizeof(buf));
            wl_display_cancel_read(display_);
            break;
        }

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(display_) < 0) {
                break;
            }
        } else {
            wl_display_cancel_read(display_);
        }

        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            wl_display_dispatch_pending(display_);
        }
    }
}

} // namespace brodisplays
