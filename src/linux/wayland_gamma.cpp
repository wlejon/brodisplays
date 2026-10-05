// Night light on wlroots compositors: zwlr_gamma_control_manager_v1.
#include "wayland_internal.h"

#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/color_temperature.h"

namespace brodisplays {

struct WaylandBackend::GammaListeners {
    static void gamma_size(void* data, zwlr_gamma_control_v1*, uint32_t size) {
        static_cast<GammaControl*>(data)->size = size;
    }
    static void failed(void* data, zwlr_gamma_control_v1*) {
        static_cast<GammaControl*>(data)->failed = true;
    }
};

namespace {

// A sealed-size memfd holding a gamma table: `size` red, then green, then
// blue 16-bit entries, a linear ramp scaled by the channel gains.
int ramp_fd(uint32_t size, double gr, double gg, double gb) {
    std::vector<uint16_t> table(static_cast<size_t>(size) * 3);
    for (uint32_t i = 0; i < size; ++i) {
        const double v = size > 1 ? static_cast<double>(i) / (size - 1) : 1.0;
        table[i] = static_cast<uint16_t>(std::lround(v * gr * 65535.0));
        table[size + i] = static_cast<uint16_t>(std::lround(v * gg * 65535.0));
        table[2 * size + i] = static_cast<uint16_t>(std::lround(v * gb * 65535.0));
    }
    int fd = memfd_create("brodisplays-gamma", MFD_CLOEXEC);
    if (fd < 0) return -1;
    const size_t bytes = table.size() * sizeof(uint16_t);
    const auto* p = reinterpret_cast<const char*>(table.data());
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = write(fd, p + done, bytes - done);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += static_cast<size_t>(n);
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

} // namespace

void WaylandBackend::drop_gamma(OutputData* od) {
    if (!od->gamma) return;
    if (od->gamma->control) zwlr_gamma_control_v1_destroy(od->gamma->control);
    od->gamma.reset();
}

bool WaylandBackend::probe_gamma(std::unique_lock<std::recursive_mutex>& lock) {
    static const zwlr_gamma_control_v1_listener kListener = {GammaListeners::gamma_size, GammaListeners::failed};
    if (!gamma_manager_) return true;
    std::vector<std::pair<OutputData*, std::unique_ptr<GammaControl>>> probes;
    for (auto* od : outputs_) {
        if (od->gamma_usable != -1 || !od->done) continue;
        if (od->gamma && !od->gamma->failed) {
            od->gamma_usable = 1;
            continue;
        }
        auto g = std::make_unique<GammaControl>();
        g->control = zwlr_gamma_control_manager_v1_get_gamma_control(gamma_manager_, od->output);
        zwlr_gamma_control_v1_add_listener(g->control, &kListener, g.get());
        probes.emplace_back(od, std::move(g));
    }
    if (probes.empty()) return true;
    // gamma_size or failed arrives before the sync's reply. The probe never
    // sets a ramp, so letting go of it changes nothing on screen.
    const bool answered = roundtrip(lock);
    for (auto& [od, g] : probes) {
        if (answered) od->gamma_usable = (!g->failed && g->size > 0) ? 1 : 0;
        if (g->control) zwlr_gamma_control_v1_destroy(g->control);
    }
    return answered;
}

bool WaylandBackend::gamma_supported() {
    std::unique_lock<std::recursive_mutex> lock(state_mutex_);
    if (!gamma_manager_ || broken_) return false;
    if (!probe_gamma(lock)) return false;
    for (const auto* od : outputs_) {
        if (od->gamma_usable == 1 || (od->gamma && !od->gamma->failed)) return true;
    }
    return false;
}

Result WaylandBackend::set_night_light(bool enabled, uint32_t temperature_kelvin) {
    static const zwlr_gamma_control_v1_listener kListener = {GammaListeners::gamma_size, GammaListeners::failed};
    std::unique_lock<std::recursive_mutex> lock(state_mutex_);
    if (broken_) return Result::failure("Wayland compositor connection is not usable");
    if (!gamma_manager_) {
        return Result::failure("The Wayland compositor offers no gamma control (wlr-gamma-control-unstable-v1)");
    }

    if (!enabled) {
        // Destroying the controls hands the outputs back; the compositor
        // restores the ramps they had before.
        for (auto* od : outputs_) drop_gamma(od);
        night_light_on_ = false;
        night_light_kelvin_ = 6500;
        if (!roundtrip(lock)) return Result::failure("The Wayland compositor stopped answering");
        return Result::success();
    }

    for (auto* od : outputs_) {
        if (od->gamma && !od->gamma->failed) continue;
        drop_gamma(od);
        od->gamma = std::make_unique<GammaControl>();
        od->gamma->control = zwlr_gamma_control_manager_v1_get_gamma_control(gamma_manager_, od->output);
        zwlr_gamma_control_v1_add_listener(od->gamma->control, &kListener, od->gamma.get());
    }
    // Each control's gamma_size (or failed) arrives before the sync's reply.
    if (!roundtrip(lock)) return Result::failure("The Wayland compositor stopped answering");

    double gr = 1, gg = 1, gb = 1;
    kelvin_to_rgb(temperature_kelvin, gr, gg, gb);
    for (auto* od : outputs_) {
        GammaControl* g = od->gamma.get();
        if (!g || g->failed || g->size == 0) continue;
        int fd = ramp_fd(g->size, gr, gg, gb);
        if (fd < 0) return Result::failure("Could not create the gamma table");
        zwlr_gamma_control_v1_set_gamma(g->control, fd);
        close(fd);
    }
    // A rejected table fails its control; that shows after another sync.
    if (!roundtrip(lock)) return Result::failure("The Wayland compositor stopped answering");

    size_t applied = 0;
    for (auto* od : outputs_) {
        if (od->gamma && !od->gamma->failed && od->gamma->size > 0) {
            ++applied;
        } else {
            drop_gamma(od);
        }
    }
    if (applied == 0) {
        night_light_on_ = false;
        return Result::failure("The compositor refused gamma control on every output "
                               "(another client, such as a night-light daemon, may hold it, "
                               "or the outputs have no gamma ramp)");
    }
    night_light_on_ = true;
    night_light_kelvin_ = temperature_kelvin;
    return Result::success();
}

} // namespace brodisplays
