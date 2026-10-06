#include "host_displays_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace brodisplays::api {

namespace {

struct ScheduleConfig {
    uint32_t start_hour = 21;
    uint32_t start_minute = 0;
    uint32_t end_hour = 7;
    uint32_t end_minute = 0;
};

std::mutex g_nightlight_mu;
ScheduleConfig g_schedule;
bool g_custom_state_set = false;
bool g_stored_enabled = false;
uint32_t g_stored_temp = 4500;

} // namespace

void installNightLightOnto(Value displaysObj) {
    ObjectBuilder displays(displaysObj);

    // bro.displays.getNightLight() -> Object
    displays.def("getNightLight", 0, [](Value, std::span<const Value>) -> Value {
        auto service = activeDisplayService();
        bool supported = false;
        bool enabled = false;
        uint32_t temp = 6500;

        if (service) {
            auto snap = service->snapshot();
            if (!snap.displays.empty()) {
                supported = snap.displays.front().night_light.supported;
                enabled = snap.displays.front().night_light.enabled;
                temp = snap.displays.front().night_light.temperature_kelvin;
            }
        }

        ScheduleConfig sched;
        {
            std::lock_guard lock(g_nightlight_mu);
            sched = g_schedule;
            if (g_custom_state_set) {
                enabled = g_stored_enabled;
                temp = g_stored_temp;
            }
        }

        ObjectBuilder b;
        b.set("supported", supported);
        b.set("enabled", enabled);
        b.set("temperature", temp);
        b.set("temperatureKelvin", temp);
        b.set("temperature_kelvin", temp);

        ObjectBuilder s;
        s.set("startHour", sched.start_hour);
        s.set("start_hour", sched.start_hour);
        s.set("startMinute", sched.start_minute);
        s.set("start_minute", sched.start_minute);
        s.set("endHour", sched.end_hour);
        s.set("end_hour", sched.end_hour);
        s.set("endMinute", sched.end_minute);
        s.set("end_minute", sched.end_minute);
        b.set("schedule", s.build());

        return b.build();
    });

    // bro.displays.setNightLight(config) -> boolean
    displays.def("setNightLight", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (reader.count() == 0) return ev::fromBool(false);

        bool enabled = false;
        uint32_t temp = 4500;
        bool hasSchedule = false;
        ScheduleConfig sched;

        if (reader.isObject(0)) {
            ev::Persistent obj(reader.get(0));
            if (ArgReader::hasProp(obj.get(), "enabled")) {
                enabled = ArgReader::getPropBool(obj.get(), "enabled");
            } else {
                enabled = true;
            }

            if (ArgReader::hasProp(obj.get(), "temperature")) {
                temp = ArgReader::getPropUint(obj.get(), "temperature", 4500);
            } else if (ArgReader::hasProp(obj.get(), "temperatureKelvin")) {
                temp = ArgReader::getPropUint(obj.get(), "temperatureKelvin", 4500);
            } else if (ArgReader::hasProp(obj.get(), "temperature_kelvin")) {
                temp = ArgReader::getPropUint(obj.get(), "temperature_kelvin", 4500);
            }

            if (ArgReader::hasProp(obj.get(), "schedule")) {
                ev::Persistent sObj(ArgReader::getProp(obj.get(), "schedule"));
                if (ev::isObject(sObj.get())) {
                    hasSchedule = true;
                    if (ArgReader::hasProp(sObj.get(), "startHour")) {
                        sched.start_hour = ArgReader::getPropUint(sObj.get(), "startHour");
                    } else if (ArgReader::hasProp(sObj.get(), "start_hour")) {
                        sched.start_hour = ArgReader::getPropUint(sObj.get(), "start_hour");
                    }
                    if (ArgReader::hasProp(sObj.get(), "startMinute")) {
                        sched.start_minute = ArgReader::getPropUint(sObj.get(), "startMinute");
                    } else if (ArgReader::hasProp(sObj.get(), "start_minute")) {
                        sched.start_minute = ArgReader::getPropUint(sObj.get(), "start_minute");
                    }
                    if (ArgReader::hasProp(sObj.get(), "endHour")) {
                        sched.end_hour = ArgReader::getPropUint(sObj.get(), "endHour");
                    } else if (ArgReader::hasProp(sObj.get(), "end_hour")) {
                        sched.end_hour = ArgReader::getPropUint(sObj.get(), "end_hour");
                    }
                    if (ArgReader::hasProp(sObj.get(), "endMinute")) {
                        sched.end_minute = ArgReader::getPropUint(sObj.get(), "endMinute");
                    } else if (ArgReader::hasProp(sObj.get(), "end_minute")) {
                        sched.end_minute = ArgReader::getPropUint(sObj.get(), "end_minute");
                    }
                }
            }
        } else if (reader.isBool(0)) {
            enabled = reader.getBool(0);
            if (reader.has(1)) {
                temp = reader.getUint(1, 4500);
            }
        }

        // Clamp temp to reasonable color temperature range (1000K - 10000K)
        temp = std::clamp(temp, 1000u, 10000u);

        {
            std::lock_guard lock(g_nightlight_mu);
            g_custom_state_set = true;
            g_stored_enabled = enabled;
            g_stored_temp = temp;
            if (hasSchedule) {
                g_schedule = sched;
            }
        }

        auto service = activeDisplayService();
        if (!service) return ev::fromBool(true);

        Result res = service->set_night_light(enabled, temp);
        return ev::fromBool(res.ok);
    });
}

} // namespace brodisplays::api
