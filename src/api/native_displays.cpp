#include "host_displays_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace brodisplays::api {

namespace {

std::mutex g_display_mu;

// Stored listener
struct DisplayListener {
    uint64_t id = 0;
    std::string event_type; // e.g. "change", "countdown", "reverted", "confirmed", "brightness", "*"
    std::shared_ptr<ev::Persistent> callback;
};

uint64_t g_next_listener_id = 1;
std::vector<DisplayListener> g_listeners;

// Pending test-then-revert promise
std::shared_ptr<ev::Persistent> g_pending_test_promise;

std::string orientationToString(DisplayOrientation o) {
    switch (o) {
        case DisplayOrientation::Rotate90: return "rotate90";
        case DisplayOrientation::Rotate180: return "rotate180";
        case DisplayOrientation::Rotate270: return "rotate270";
        case DisplayOrientation::Normal:
        default:
            return "normal";
    }
}

uint32_t orientationToDegrees(DisplayOrientation o) {
    return static_cast<uint32_t>(o);
}

std::optional<DisplayOrientation> parseOrientation(Value v) {
    ev::Persistent vP(v);
    if (ev::isNumber(vP.get())) {
        double d = ev::toDouble(vP.get());
        if (d == 90.0) return DisplayOrientation::Rotate90;
        if (d == 180.0) return DisplayOrientation::Rotate180;
        if (d == 270.0) return DisplayOrientation::Rotate270;
        return DisplayOrientation::Normal;
    }
    if (ev::isString(vP.get())) {
        std::string s = ev::toUtf8(vP.get());
        if (s == "90" || s == "rotate90" || s == "right") return DisplayOrientation::Rotate90;
        if (s == "180" || s == "rotate180" || s == "inverted") return DisplayOrientation::Rotate180;
        if (s == "270" || s == "rotate270" || s == "left") return DisplayOrientation::Rotate270;
        return DisplayOrientation::Normal;
    }
    return std::nullopt;
}

} // namespace

Value displayModeToJs(const DisplayMode& mode) {
    ObjectBuilder b;
    b.set("width", mode.width);
    b.set("height", mode.height);
    b.set("refreshRate", mode.refresh_rate);
    b.set("refresh_rate", mode.refresh_rate);
    return b.build();
}

Value edidInfoToJs(const EdidInfo& edid) {
    ObjectBuilder b;
    b.set("manufacturerId", edid.manufacturer_id);
    b.set("manufacturer_id", edid.manufacturer_id);
    b.set("productCode", edid.product_code);
    b.set("product_code", edid.product_code);
    b.set("serialNumber", edid.serial_number);
    b.set("serial_number", edid.serial_number);
    b.set("monitorName", edid.monitor_name);
    b.set("monitor_name", edid.monitor_name);
    b.set("widthCm", edid.width_cm);
    b.set("width_cm", edid.width_cm);
    b.set("heightCm", edid.height_cm);
    b.set("height_cm", edid.height_cm);
    return b.build();
}

Value displayInfoToJs(const DisplayInfo& info) {
    ObjectBuilder b;
    b.set("id", info.id);
    b.set("name", info.name);
    b.set("adapterName", info.adapter_name);
    b.set("adapter_name", info.adapter_name);
    b.set("manufacturer", info.manufacturer);
    b.set("model", info.model);
    b.set("deviceName", info.device_name);
    b.set("device_name", info.device_name);
    b.set("isPrimary", info.is_primary);
    b.set("is_primary", info.is_primary);
    b.set("isInternal", info.is_internal);
    b.set("is_internal", info.is_internal);
    b.set("isConnected", info.is_connected);
    b.set("is_connected", info.is_connected);
    b.set("isActive", info.is_active);
    b.set("is_active", info.is_active);
    b.set("mirrorOf", info.mirror_of);
    b.set("mirror_of", info.mirror_of);

    // Geometry & position
    {
        ObjectBuilder geom;
        geom.set("x", info.geometry.x);
        geom.set("y", info.geometry.y);
        geom.set("width", info.geometry.width);
        geom.set("height", info.geometry.height);
        b.set("geometry", geom.build());

        ObjectBuilder pos;
        pos.set("x", info.geometry.x);
        pos.set("y", info.geometry.y);
        b.set("position", pos.build());
    }

    b.set("physicalWidthMm", info.physical_width_mm);
    b.set("physical_width_mm", info.physical_width_mm);
    b.set("physicalHeightMm", info.physical_height_mm);
    b.set("physical_height_mm", info.physical_height_mm);

    b.set("orientation", orientationToString(info.orientation));
    b.set("rotation", orientationToDegrees(info.orientation));
    b.set("transform", orientationToString(info.orientation));

    // Scale
    {
        ObjectBuilder scale;
        scale.set("factor", info.scale.factor);
        scale.set("dpi", info.scale.dpi);
        b.set("scale", scale.build());
        b.set("scaleFactor", info.scale.factor);
        b.set("scale_factor", info.scale.factor);
    }

    // Modes
    b.set("currentMode", displayModeToJs(info.current_mode));
    b.set("current_mode", displayModeToJs(info.current_mode));

    {
        ev::Persistent modesArr(ev::makeArray(static_cast<uint32_t>(info.available_modes.size())));
        for (uint32_t i = 0; i < info.available_modes.size(); ++i) {
            ev::Persistent item(displayModeToJs(info.available_modes[i]));
            ev::setElement(modesArr.get(), i, item.get());
        }
        b.set("availableModes", modesArr.get());
        b.set("available_modes", modesArr.get());
        b.set("modes", modesArr.get());
    }

    // HDR
    {
        ObjectBuilder hdr;
        hdr.set("supported", info.hdr.supported);
        hdr.set("enabled", info.hdr.enabled);
        hdr.set("maxLuminanceNits", static_cast<double>(info.hdr.max_luminance_nits));
        hdr.set("minLuminanceNits", static_cast<double>(info.hdr.min_luminance_nits));
        b.set("hdr", hdr.build());
    }

    // Night light status
    {
        ObjectBuilder nl;
        nl.set("supported", info.night_light.supported);
        nl.set("enabled", info.night_light.enabled);
        nl.set("temperature", info.night_light.temperature_kelvin);
        nl.set("temperatureKelvin", info.night_light.temperature_kelvin);
        b.set("nightLight", nl.build());
        b.set("night_light", nl.build());
    }

    // Color profile
    {
        ObjectBuilder cp;
        cp.set("name", info.color_profile.name);
        cp.set("iccPath", info.color_profile.icc_path);
        cp.set("icc_path", info.color_profile.icc_path);
        b.set("colorProfile", cp.build());
        b.set("color_profile", cp.build());
    }

    // EDID
    b.set("edid", edidInfoToJs(info.edid));

    return b.build();
}

Value displaySnapshotToJs(const DisplaysSnapshot& snap) {
    ObjectBuilder b;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        snap.timestamp.time_since_epoch()).count();
    b.set("timestamp", static_cast<double>(ms));

    ev::Persistent displaysArr(ev::makeArray(static_cast<uint32_t>(snap.displays.size())));
    for (uint32_t i = 0; i < snap.displays.size(); ++i) {
        ev::Persistent item(displayInfoToJs(snap.displays[i]));
        ev::setElement(displaysArr.get(), i, item.get());
    }
    b.set("displays", displaysArr.get());

    return b.build();
}

void clearDisplayListeners() {
    std::lock_guard lock(g_display_mu);
    g_listeners.clear();
}

void revertPendingTestIfAny() {
    std::shared_ptr<ev::Persistent> testP;
    {
        std::lock_guard lock(g_display_mu);
        testP = std::move(g_pending_test_promise);
    }
    auto service = activeDisplayService();
    if (service && service->is_revert_pending()) {
        service->revert_configuration();
    }
    if (testP && !ev::isUndefined(testP->get())) {
        ev::resolvePromise(testP->get(), ev::fromBool(false));
    }
}

void dispatchListenerEvent(const std::string& type, Value eventPayload) {
    std::vector<std::shared_ptr<ev::Persistent>> targets;
    {
        std::lock_guard lock(g_display_mu);
        for (const auto& l : g_listeners) {
            if (l.event_type == type || l.event_type == "*" ||
                (type == "change" && l.event_type == "displaysChanged") ||
                (type == "countdown" && l.event_type == "revertCountdown") ||
                (type == "reverted" && l.event_type == "configurationReverted") ||
                (type == "confirmed" && l.event_type == "configurationConfirmed") ||
                (type == "brightness" && l.event_type == "brightnessChanged")) {
                targets.push_back(l.callback);
            }
        }
    }

    ev::Persistent payloadP(eventPayload);
    for (const auto& cb : targets) {
        if (cb && ev::isFunction(cb->get())) {
            Value arg = payloadP.get();
            ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1));
        }
    }
}

void drainDisplayEvents() {
    auto service = activeDisplayService();
    if (!service) return;

    auto events = service->events().drain();
    for (const auto& evItem : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, DisplaysChanged>) {
                ev::Persistent snapJs(displaySnapshotToJs(e.snapshot));
                dispatchListenerEvent("change", snapJs.get());
            } else if constexpr (std::is_same_v<T, RevertCountdown>) {
                ObjectBuilder cd;
                cd.set("type", "countdown");
                cd.set("displayId", e.display_id);
                cd.set("display_id", e.display_id);
                cd.set("remainingMs", static_cast<double>(e.remaining.count()));
                cd.set("remaining_ms", static_cast<double>(e.remaining.count()));
                dispatchListenerEvent("countdown", cd.build());
            } else if constexpr (std::is_same_v<T, ConfigurationConfirmed>) {
                std::shared_ptr<ev::Persistent> testP;
                {
                    std::lock_guard lock(g_display_mu);
                    testP = std::move(g_pending_test_promise);
                }
                if (testP && !ev::isUndefined(testP->get())) {
                    ev::resolvePromise(testP->get(), ev::fromBool(true));
                }

                ObjectBuilder cf;
                cf.set("type", "confirmed");
                cf.set("displayId", e.display_id);
                cf.set("display_id", e.display_id);
                dispatchListenerEvent("confirmed", cf.build());
            } else if constexpr (std::is_same_v<T, ConfigurationReverted>) {
                std::shared_ptr<ev::Persistent> testP;
                {
                    std::lock_guard lock(g_display_mu);
                    testP = std::move(g_pending_test_promise);
                }
                if (testP && !ev::isUndefined(testP->get())) {
                    ev::resolvePromise(testP->get(), ev::fromBool(false));
                }

                ObjectBuilder rv;
                rv.set("type", "reverted");
                rv.set("displayId", e.display_id);
                rv.set("display_id", e.display_id);
                rv.set("reason", e.reason);
                rv.set("restored", e.restored);
                rv.set("error", e.error);
                dispatchListenerEvent("reverted", rv.build());
            } else if constexpr (std::is_same_v<T, BrightnessEvent>) {
                ObjectBuilder be;
                be.set("type", "brightness");
                be.set("deviceName", e.device_name);
                be.set("device_name", e.device_name);
                be.set("connectorName", e.connector_name);
                be.set("connector_name", e.connector_name);
                be.set("rawBrightness", e.raw_brightness);
                be.set("raw_brightness", e.raw_brightness);
                be.set("maxBrightness", e.max_brightness);
                be.set("max_brightness", e.max_brightness);
                be.set("minBrightness", e.min_brightness);
                be.set("min_brightness", e.min_brightness);
                be.set("normalizedBrightness", e.normalized_brightness);
                be.set("normalized_brightness", e.normalized_brightness);
                be.set("percent", e.normalized_brightness * 100.0);
                dispatchListenerEvent("brightness", be.build());
            }
        }, evItem);
    }
}

DisplayConfigChange parseConfigChange(Value cfgObj, std::shared_ptr<DisplayService> service) {
    DisplayConfigChange change;
    if (!ev::isObject(cfgObj)) return change;
    ev::Persistent cfg(cfgObj);

    std::string id = ArgReader::getPropString(cfg.get(), "displayId");
    if (id.empty()) id = ArgReader::getPropString(cfg.get(), "display_id");
    if (id.empty()) id = ArgReader::getPropString(cfg.get(), "id");

    if (id.empty() && service) {
        auto snap = service->snapshot();
        const auto* primary = snap.primary_display();
        if (primary) id = primary->id;
    }
    change.display_id = id;

    if (ArgReader::hasProp(cfg.get(), "width")) {
        change.width = ArgReader::getPropUint(cfg.get(), "width");
    }
    if (ArgReader::hasProp(cfg.get(), "height")) {
        change.height = ArgReader::getPropUint(cfg.get(), "height");
    }

    if (ArgReader::hasProp(cfg.get(), "refreshRate")) {
        change.refresh_rate = ArgReader::getPropDouble(cfg.get(), "refreshRate");
    } else if (ArgReader::hasProp(cfg.get(), "refresh_rate")) {
        change.refresh_rate = ArgReader::getPropDouble(cfg.get(), "refresh_rate");
    }

    if (ArgReader::hasProp(cfg.get(), "x")) {
        change.x = ArgReader::getPropInt(cfg.get(), "x");
    }
    if (ArgReader::hasProp(cfg.get(), "y")) {
        change.y = ArgReader::getPropInt(cfg.get(), "y");
    }

    if (ArgReader::hasProp(cfg.get(), "orientation")) {
        change.orientation = parseOrientation(ArgReader::getProp(cfg.get(), "orientation"));
    } else if (ArgReader::hasProp(cfg.get(), "rotation")) {
        change.orientation = parseOrientation(ArgReader::getProp(cfg.get(), "rotation"));
    }

    if (ArgReader::hasProp(cfg.get(), "scale")) {
        change.scale_factor = ArgReader::getPropDouble(cfg.get(), "scale");
    } else if (ArgReader::hasProp(cfg.get(), "scaleFactor")) {
        change.scale_factor = ArgReader::getPropDouble(cfg.get(), "scaleFactor");
    } else if (ArgReader::hasProp(cfg.get(), "scale_factor")) {
        change.scale_factor = ArgReader::getPropDouble(cfg.get(), "scale_factor");
    }

    return change;
}

void installDisplaysOnto(Value displaysObj) {
    ObjectBuilder displays(displaysObj);

    // bro.displays.getSnapshot() -> Object
    displays.def("getSnapshot", 0, [](Value, std::span<const Value>) -> Value {
        auto service = activeDisplayService();
        if (!service) {
            DisplaysSnapshot empty;
            return displaySnapshotToJs(empty);
        }
        auto snap = service->snapshot();
        return displaySnapshotToJs(snap);
    });

    // bro.displays.applyConfig(config) -> Promise<boolean>
    displays.def("applyConfig", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        ev::Persistent promiseP(ev::createPromise());

        if (!reader.isObject(0)) {
            ev::Persistent err(makeError("bro.displays.applyConfig requires a config object"));
            ev::rejectPromise(promiseP.get(), err.get());
            return promiseP.get();
        }

        auto service = activeDisplayService();
        if (!service) {
            ev::Persistent err(makeError("Display service is unavailable"));
            ev::rejectPromise(promiseP.get(), err.get());
            return promiseP.get();
        }

        DisplayConfigChange change = parseConfigChange(reader.get(0), service);
        Result res = service->apply_configuration(change);
        ev::resolvePromise(promiseP.get(), ev::fromBool(res.ok));
        return promiseP.get();
    });

    // bro.displays.testConfig(config, options?) -> Promise<boolean>
    displays.def("testConfig", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        ev::Persistent promiseP(ev::createPromise());

        if (!reader.isObject(0)) {
            ev::Persistent err(makeError("bro.displays.testConfig requires a config object"));
            ev::rejectPromise(promiseP.get(), err.get());
            return promiseP.get();
        }

        auto service = activeDisplayService();
        if (!service) {
            ev::Persistent err(makeError("Display service is unavailable"));
            ev::rejectPromise(promiseP.get(), err.get());
            return promiseP.get();
        }

        DisplayConfigChange change = parseConfigChange(reader.get(0), service);

        // Options: revertAfterMs / timeoutMs / timeout
        std::chrono::milliseconds timeout = std::chrono::seconds(10);
        if (reader.isObject(1)) {
            ev::Persistent opt(reader.get(1));
            if (ArgReader::hasProp(opt.get(), "revertAfterMs")) {
                double ms = ArgReader::getPropDouble(opt.get(), "revertAfterMs");
                if (ms > 0) timeout = std::chrono::milliseconds(static_cast<int64_t>(ms));
            } else if (ArgReader::hasProp(opt.get(), "timeoutMs")) {
                double ms = ArgReader::getPropDouble(opt.get(), "timeoutMs");
                if (ms > 0) timeout = std::chrono::milliseconds(static_cast<int64_t>(ms));
            } else if (ArgReader::hasProp(opt.get(), "timeout")) {
                double ms = ArgReader::getPropDouble(opt.get(), "timeout");
                if (ms > 0) timeout = std::chrono::milliseconds(static_cast<int64_t>(ms));
            }
        }

        Result res = service->apply_temporary_configuration(change, timeout);
        if (!res.ok) {
            ev::resolvePromise(promiseP.get(), ev::fromBool(false));
            return promiseP.get();
        }

        {
            std::lock_guard lock(g_display_mu);
            if (g_pending_test_promise && !ev::isUndefined(g_pending_test_promise->get())) {
                ev::resolvePromise(g_pending_test_promise->get(), ev::fromBool(false));
            }
            g_pending_test_promise = std::make_shared<ev::Persistent>(promiseP.get());
        }

        return promiseP.get();
    });

    // bro.displays.confirmConfig() -> boolean
    displays.def("confirmConfig", 0, [](Value, std::span<const Value>) -> Value {
        auto service = activeDisplayService();
        if (!service) return ev::fromBool(false);
        Result res = service->confirm_configuration();
        if (res.ok) {
            std::shared_ptr<ev::Persistent> testP;
            {
                std::lock_guard lock(g_display_mu);
                testP = std::move(g_pending_test_promise);
            }
            if (testP && !ev::isUndefined(testP->get())) {
                ev::resolvePromise(testP->get(), ev::fromBool(true));
            }
        }
        return ev::fromBool(res.ok);
    });

    // bro.displays.revertConfig() -> boolean
    displays.def("revertConfig", 0, [](Value, std::span<const Value>) -> Value {
        auto service = activeDisplayService();
        if (!service) return ev::fromBool(false);
        Result res = service->revert_configuration();
        if (res.ok) {
            std::shared_ptr<ev::Persistent> testP;
            {
                std::lock_guard lock(g_display_mu);
                testP = std::move(g_pending_test_promise);
            }
            if (testP && !ev::isUndefined(testP->get())) {
                ev::resolvePromise(testP->get(), ev::fromBool(false));
            }
        }
        return ev::fromBool(res.ok);
    });

    // bro.displays.isRevertPending() -> boolean
    displays.def("isRevertPending", 0, [](Value, std::span<const Value>) -> Value {
        auto service = activeDisplayService();
        if (!service) return ev::fromBool(false);
        return ev::fromBool(service->is_revert_pending());
    });

    // bro.displays.on(event, callback) -> ListenerToken
    auto onFn = [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.isString(0) || !reader.isFunction(1)) {
            return ev::fromDouble(0.0);
        }

        std::string event = reader.getString(0);
        uint64_t id = 0;
        {
            std::lock_guard lock(g_display_mu);
            id = g_next_listener_id++;
            DisplayListener l;
            l.id = id;
            l.event_type = event;
            l.callback = std::make_shared<ev::Persistent>(reader.get(1));
            g_listeners.push_back(std::move(l));
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(id));
        handle.set("event", event);
        handle.def("remove", 0, [id](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_display_mu);
            std::erase_if(g_listeners, [id](const auto& l) { return l.id == id; });
            return ev::fromBool(true);
        });
        return handle.build();
    };

    displays.def("on", 2, onFn);
    displays.def("addEventListener", 2, onFn);
    displays.def("addListener", 2, onFn);

    // bro.displays.off(eventOrHandle, callback?) -> boolean
    auto offFn = [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (reader.count() == 0) return ev::fromBool(false);

        std::lock_guard lock(g_display_mu);
        if (reader.isObject(0)) {
            ev::Persistent objP(reader.get(0));
            double id = ArgReader::getPropDouble(objP.get(), "id", 0.0);
            if (id > 0) {
                uint64_t targetId = static_cast<uint64_t>(id);
                size_t removed = std::erase_if(g_listeners, [targetId](const auto& l) {
                    return l.id == targetId;
                });
                return ev::fromBool(removed > 0);
            }
        }

        if (reader.isString(0)) {
            std::string event = reader.getString(0);
            if (reader.isFunction(1)) {
                Value targetCb = reader.get(1);
                size_t removed = std::erase_if(g_listeners, [&](const auto& l) {
                    return l.event_type == event && l.callback &&
                           (l.callback->get() == targetCb);
                });
                return ev::fromBool(removed > 0);
            }
            // Remove all for this event type
            size_t removed = std::erase_if(g_listeners, [&](const auto& l) {
                return l.event_type == event;
            });
            return ev::fromBool(removed > 0);
        }

        return ev::fromBool(false);
    };

    displays.def("off", 1, offFn);
    displays.def("removeEventListener", 1, offFn);
    displays.def("removeListener", 1, offFn);
}

} // namespace brodisplays::api
