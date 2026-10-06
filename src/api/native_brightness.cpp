#include "host_displays_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

namespace brodisplays::api {

namespace {

std::mutex g_backlight_mu;
std::unique_ptr<brodisplays::BacklightWatcher> g_backlight_watcher;

void ensureBacklightWatcherStarted() {
    std::lock_guard lock(g_backlight_mu);
    if (g_backlight_watcher) return;

    auto mgr = activeBacklightManager();
    auto service = activeDisplayService();
    if (mgr && service) {
        try {
            g_backlight_watcher = mgr->create_watcher(service->events());
        } catch (...) {
            // Watcher creation not supported on platforms without inotify / backlight
        }
    }
}

Value backlightDeviceToJs(const BacklightDevice& dev) {
    ObjectBuilder b;
    b.set("name", dev.name);
    b.set("sysfsPath", dev.sysfs_path);
    b.set("sysfs_path", dev.sysfs_path);
    b.set("type", std::string(to_string(dev.type)));
    b.set("currentBrightness", dev.current_brightness);
    b.set("current_brightness", dev.current_brightness);
    b.set("maxBrightness", dev.max_brightness);
    b.set("max_brightness", dev.max_brightness);
    b.set("minBrightness", dev.min_brightness);
    b.set("min_brightness", dev.min_brightness);
    b.set("normalizedBrightness", dev.normalized_brightness);
    b.set("normalized_brightness", dev.normalized_brightness);
    b.set("percent", dev.normalized_brightness * 100.0);
    b.set("associatedConnector", dev.associated_connector);
    b.set("associated_connector", dev.associated_connector);
    b.set("connector", dev.associated_connector);
    return b.build();
}

} // namespace

void installBrightnessOnto(Value displaysObj) {
    ObjectBuilder displays(displaysObj);

    // bro.displays.getBrightness(displayId?) -> number
    displays.def("getBrightness", 0, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        std::string target;
        if (reader.isString(0)) {
            target = reader.getString(0);
        }

        auto mgr = activeBacklightManager();
        if (!mgr) return ev::fromDouble(-1.0);

        uint32_t raw = 0;
        double normalized = 0.0;
        Result res = mgr->get_brightness(target, raw, normalized);
        if (!res.ok) {
            return ev::fromDouble(-1.0);
        }
        return ev::fromDouble(normalized * 100.0);
    });

    // bro.displays.setBrightness(displayId, percent) or (percent) -> boolean
    displays.def("setBrightness", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (reader.count() == 0) return ev::fromBool(false);

        std::string target;
        double percent = 100.0;

        if (reader.count() >= 2) {
            target = reader.getString(0);
            percent = reader.getDouble(1, 100.0);
        } else {
            if (reader.isNumber(0)) {
                percent = reader.getDouble(0, 100.0);
            } else if (reader.isString(0)) {
                target = reader.getString(0);
            }
        }

        double normalized = percent;
        if (percent > 1.0) {
            normalized = percent / 100.0;
        }
        normalized = std::clamp(normalized, 0.0, 1.0);

        auto mgr = activeBacklightManager();
        if (!mgr) return ev::fromBool(false);

        ensureBacklightWatcherStarted();

        Result res = mgr->set_normalized_brightness(target, normalized);
        return ev::fromBool(res.ok);
    });

    // bro.displays.getBacklightDevices() -> BacklightDevice[]
    displays.def("getBacklightDevices", 0, [](Value, std::span<const Value>) -> Value {
        auto mgr = activeBacklightManager();
        if (!mgr) return ev::makeArray(0);

        auto devices = mgr->enumerate_devices();
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(devices.size())));
        for (uint32_t i = 0; i < devices.size(); ++i) {
            ev::Persistent item(backlightDeviceToJs(devices[i]));
            ev::setElement(arr.get(), i, item.get());
        }
        return arr.get();
    });
}

} // namespace brodisplays::api
