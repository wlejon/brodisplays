#include "desktop_night_light.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#ifdef BRODISPLAYS_HAVE_GIO
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include <gio/gio.h>
#endif

namespace brodisplays {

bool desktop_owns_gamma() {
    const char* d = std::getenv("XDG_CURRENT_DESKTOP");
    if (!d) return false;
    std::string s = d;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s.find("KDE") != std::string::npos || s.find("GNOME") != std::string::npos;
}

#ifndef BRODISPLAYS_HAVE_GIO

std::unique_ptr<DesktopNightLight> DesktopNightLight::detect() {
    return nullptr;
}

#else

namespace {

// ---------------------------------------------------------------- GIO helpers

struct Var {
    GVariant* v = nullptr;
    Var() = default;
    explicit Var(GVariant* p) : v(p) {}
    Var(Var&& o) noexcept : v(o.v) { o.v = nullptr; }
    Var& operator=(Var&& o) noexcept {
        if (this != &o) {
            if (v) g_variant_unref(v);
            v = o.v;
            o.v = nullptr;
        }
        return *this;
    }
    ~Var() {
        if (v) g_variant_unref(v);
    }
    explicit operator bool() const { return v != nullptr; }
};

// A private session-bus connection (never the process-wide singleton, which
// would exit the process if the bus went away).
GDBusConnection* open_session_bus() {
    GError* err = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!address) {
        g_clear_error(&err);
        return nullptr;
    }
    GDBusConnection* c = g_dbus_connection_new_for_address_sync(
        address,
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &err);
    g_free(address);
    g_clear_error(&err);
    if (c) g_dbus_connection_set_exit_on_close(c, FALSE);
    return c;
}

Var call(GDBusConnection* c, const char* dest, const char* path, const char* iface, const char* method,
         GVariant* args, const char* reply_type) {
    GError* err = nullptr;
    GVariant* r = g_dbus_connection_call_sync(c, dest, path, iface, method, args,
                                              reply_type ? G_VARIANT_TYPE(reply_type) : nullptr,
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, DesktopNightLight::kTimeoutMs, nullptr,
                                              &err);
    g_clear_error(&err);
    return Var(r);
}

Var get_property(GDBusConnection* c, const char* dest, const char* path, const char* iface, const char* prop) {
    Var r = call(c, dest, path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, prop), "(v)");
    if (!r) return Var();
    GVariant* v = nullptr;
    g_variant_get(r.v, "(v)", &v);
    return Var(v);
}

std::optional<bool> get_bool(GDBusConnection* c, const char* dest, const char* path, const char* iface,
                             const char* prop) {
    Var v = get_property(c, dest, path, iface, prop);
    if (!v || !g_variant_is_of_type(v.v, G_VARIANT_TYPE_BOOLEAN)) return std::nullopt;
    return g_variant_get_boolean(v.v) != FALSE;
}

std::optional<uint32_t> get_u32(GDBusConnection* c, const char* dest, const char* path, const char* iface,
                                const char* prop) {
    Var v = get_property(c, dest, path, iface, prop);
    if (!v || !g_variant_is_of_type(v.v, G_VARIANT_TYPE_UINT32)) return std::nullopt;
    return g_variant_get_uint32(v.v);
}

bool name_has_owner(GDBusConnection* c, const char* name) {
    Var r = call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                 g_variant_new("(s)", name), "(b)");
    if (!r) return false;
    gboolean owned = FALSE;
    g_variant_get(r.v, "(b)", &owned);
    return owned != FALSE;
}

// Polls `done` for up to `timeout` (desktops apply settings asynchronously).
template <class F>
bool settles(F done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ---------------------------------------------------------------- kwinrc

std::string config_home() {
    const char* x = std::getenv("XDG_CONFIG_HOME");
    if (x && *x) return x;
    const char* h = std::getenv("HOME");
    return std::string(h ? h : "") + "/.config";
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Sets (value) or removes (nullopt) keys of one group of a KConfig INI file,
// keeping everything else as it is; `previous` receives the values the keys
// had (nullopt: absent). Written to a temporary file and renamed into place.
bool update_kconfig(const std::string& path, const std::string& group,
                    const std::map<std::string, std::optional<std::string>>& values,
                    std::map<std::string, std::optional<std::string>>* previous) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    const std::string header = "[" + group + "]";
    size_t begin = lines.size(), end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (trim(lines[i]) == header) {
            begin = i + 1;
            end = lines.size();
            for (size_t j = begin; j < lines.size(); ++j) {
                if (!trim(lines[j]).empty() && trim(lines[j])[0] == '[') {
                    end = j;
                    break;
                }
            }
            break;
        }
    }
    if (previous) {
        for (const auto& [k, v] : values) (*previous)[k] = std::nullopt;
    }
    std::map<std::string, bool> written;
    std::vector<std::string> out;
    if (begin == lines.size()) {
        out = lines;
        if (!out.empty() && !trim(out.back()).empty()) out.push_back("");
        out.push_back(header);
        for (const auto& [k, v] : values) {
            if (v) out.push_back(k + "=" + *v);
        }
    } else {
        out.assign(lines.begin(), lines.begin() + static_cast<long>(begin));
        size_t last_content = out.size();
        for (size_t i = begin; i < end; ++i) {
            const std::string t = trim(lines[i]);
            size_t eq = t.find('=');
            std::string key = eq == std::string::npos ? std::string() : trim(t.substr(0, eq));
            auto it = values.find(key);
            if (!key.empty() && it != values.end()) {
                if (previous) (*previous)[key] = trim(t.substr(eq + 1));
                if (it->second && !written[key]) out.push_back(key + "=" + *it->second);
                written[key] = true;
            } else {
                out.push_back(lines[i]);
            }
            if (!t.empty()) last_content = out.size();
        }
        std::vector<std::string> extra;
        for (const auto& [k, v] : values) {
            if (v && !written[k]) extra.push_back(k + "=" + *v);
        }
        out.insert(out.begin() + static_cast<long>(last_content), extra.begin(), extra.end());
        out.insert(out.end(), lines.begin() + static_cast<long>(end), lines.end());
    }
    const std::string tmp = path + ".brodisplays-tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return false;
        for (const auto& l : out) o << l << '\n';
        if (!o.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// ---------------------------------------------------------------- KDE Plasma

constexpr const char* kKWin = "org.kde.KWin";
constexpr const char* kKWinPath = "/org/kde/KWin/NightLight";
constexpr const char* kKWinIface = "org.kde.KWin.NightLight";

class KWinNightLight : public DesktopNightLight {
public:
    explicit KWinNightLight(GDBusConnection* c) : bus_(c) {}
    ~KWinNightLight() override { g_object_unref(bus_); }

    const char* name() const override { return "KDE Plasma (KWin)"; }

    NightLightStatus status() override {
        NightLightStatus st;
        st.supported = get_bool(bus_, kKWin, kKWinPath, kKWinIface, "available").value_or(false);
        st.enabled = st.supported && get_bool(bus_, kKWin, kKWinPath, kKWinIface, "enabled").value_or(false);
        st.temperature_kelvin = get_u32(bus_, kKWin, kKWinPath, kKWinIface, "currentTemperature").value_or(6500);
        return st;
    }

    Result set(bool enabled, uint32_t kelvin) override {
        if (!get_bool(bus_, kKWin, kKWinPath, kKWinIface, "available").value_or(false)) {
            return Result::failure("KWin reports night light unavailable on these displays");
        }
        kelvin = std::clamp(kelvin, 1000u, 6500u);
        std::map<std::string, std::optional<std::string>> values;
        values["Active"] = std::string(enabled ? "true" : "false");
        if (enabled) {
            // Constant: on now, at this temperature, whatever the schedule.
            values["Mode"] = std::string("Constant");
            values["NightTemperature"] = std::to_string(kelvin);
        } else if (saved_mode_) {
            values["Mode"] = *saved_mode_;  // the user's own schedule mode
        }
        std::map<std::string, std::optional<std::string>> previous;
        const std::string path = config_home() + "/kwinrc";
        if (!update_kconfig(path, "NightColor", values, &previous)) {
            return Result::failure("Could not write " + path);
        }
        if (enabled && !saved_mode_) {
            saved_mode_ = previous["Mode"];
        } else if (!enabled) {
            saved_mode_.reset();
        }
        if (!notify_kwin(values)) return Result::failure("Could not announce the kwinrc change on the session bus");

        const bool reached = settles([&] {
            auto on = get_bool(bus_, kKWin, kKWinPath, kKWinIface, "enabled");
            if (!on || *on != enabled) return false;
            if (!enabled) return true;
            return get_u32(bus_, kKWin, kKWinPath, kKWinIface, "targetTemperature").value_or(0) == kelvin;
        }, std::chrono::milliseconds(3000));
        if (!reached) return Result::failure("KWin did not take the night light setting");
        return Result::success();
    }

private:
    // What kwriteconfig6 --notify does: org.kde.kconfig.notify ConfigChanged
    // on /kwinrc with {group: [keys]}; KWin's config watcher reloads on it.
    bool notify_kwin(const std::map<std::string, std::optional<std::string>>& values) {
        GVariantBuilder keys;
        g_variant_builder_init(&keys, G_VARIANT_TYPE("aay"));
        for (const auto& [k, v] : values) {
            g_variant_builder_add_value(&keys, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, k.data(), k.size(), 1));
        }
        GVariantBuilder groups;
        g_variant_builder_init(&groups, G_VARIANT_TYPE("a{saay}"));
        g_variant_builder_add(&groups, "{s@aay}", "NightColor", g_variant_builder_end(&keys));
        GError* err = nullptr;
        gboolean ok = g_dbus_connection_emit_signal(bus_, nullptr, "/kwinrc", "org.kde.kconfig.notify",
                                                    "ConfigChanged", g_variant_new("(@a{saay})",
                                                                                   g_variant_builder_end(&groups)),
                                                    &err);
        g_clear_error(&err);
        if (ok) ok = g_dbus_connection_flush_sync(bus_, nullptr, &err);
        g_clear_error(&err);
        return ok != FALSE;
    }

    GDBusConnection* bus_;
    std::optional<std::optional<std::string>> saved_mode_;
};

// ---------------------------------------------------------------- GNOME

constexpr const char* kGsdColor = "org.gnome.SettingsDaemon.Color";
constexpr const char* kGsdColorPath = "/org/gnome/SettingsDaemon/Color";
constexpr const char* kMutterConfig = "org.gnome.Mutter.DisplayConfig";
constexpr const char* kMutterConfigPath = "/org/gnome/Mutter/DisplayConfig";
constexpr const char* kColorSchema = "org.gnome.settings-daemon.plugins.color";

class GnomeNightLight : public DesktopNightLight {
public:
    GnomeNightLight(GDBusConnection* c, GSettingsSchema* schema) : bus_(c), schema_(schema) {}
    ~GnomeNightLight() override {
        if (settings_) g_object_unref(settings_);
        g_settings_schema_unref(schema_);
        g_object_unref(bus_);
    }

    const char* name() const override { return "GNOME"; }

    // mutter decides whether its outputs can show night light (it needs
    // gamma or colour transforms); gnome-settings-daemon runs the schedule.
    bool supported() {
        if (name_has_owner(bus_, kMutterConfig)) {
            return get_bool(bus_, kMutterConfig, kMutterConfigPath, kMutterConfig, "NightLightSupported")
                .value_or(false);
        }
        return name_has_owner(bus_, kGsdColor);
    }

    NightLightStatus status() override {
        NightLightStatus st;
        st.supported = supported();
        st.enabled = st.supported && get_bool(bus_, kGsdColor, kGsdColorPath, kGsdColor, "NightLightActive").value_or(false);
        st.temperature_kelvin = get_u32(bus_, kGsdColor, kGsdColorPath, kGsdColor, "Temperature").value_or(6500);
        return st;
    }

    Result set(bool enabled, uint32_t kelvin) override {
        if (!supported()) return Result::failure("GNOME reports night light unavailable on these displays");
        if (!name_has_owner(bus_, kGsdColor)) {
            return Result::failure("gnome-settings-daemon's colour plugin is not running");
        }
        settings();
        if (enabled) {
            kelvin = clamp_temperature(kelvin);
            if (!saved_schedule_) {
                saved_schedule_ = Schedule{g_settings_get_boolean(settings_, "night-light-schedule-automatic") != FALSE,
                                           g_settings_get_double(settings_, "night-light-schedule-from"),
                                           g_settings_get_double(settings_, "night-light-schedule-to")};
            }
            // A manual schedule from midnight to midnight covers the whole
            // day: night light is on now, whatever the time.
            g_settings_set_boolean(settings_, "night-light-schedule-automatic", FALSE);
            g_settings_set_double(settings_, "night-light-schedule-from", 0.0);
            g_settings_set_double(settings_, "night-light-schedule-to", 0.0);
            g_settings_set_uint(settings_, "night-light-temperature", kelvin);
            g_settings_set_boolean(settings_, "night-light-enabled", TRUE);
            // "Disable until tomorrow" would keep it off.
            call(bus_, kGsdColor, kGsdColorPath, "org.freedesktop.DBus.Properties", "Set",
                 g_variant_new("(ssv)", kGsdColor, "DisabledUntilTomorrow", g_variant_new_boolean(FALSE)), nullptr);
        } else {
            g_settings_set_boolean(settings_, "night-light-enabled", FALSE);
            if (saved_schedule_) {
                g_settings_set_boolean(settings_, "night-light-schedule-automatic", saved_schedule_->automatic);
                g_settings_set_double(settings_, "night-light-schedule-from", saved_schedule_->from);
                g_settings_set_double(settings_, "night-light-schedule-to", saved_schedule_->to);
                saved_schedule_.reset();
            }
        }
        g_settings_sync();
        const bool reached = settles([&] {
            return get_bool(bus_, kGsdColor, kGsdColorPath, kGsdColor, "NightLightActive").value_or(!enabled) == enabled;
        }, std::chrono::milliseconds(3000));
        if (!reached) return Result::failure("gnome-settings-daemon did not take the night light setting");
        return Result::success();
    }

private:
    struct Schedule {
        bool automatic;
        double from, to;
    };

    // Created on first use. GSettings' dconf backend talks over GLib's
    // shared session-bus connection, which by default raises SIGTERM in the
    // whole process when the bus goes away; a library must not do that to
    // its host, so that connection is told not to.
    GSettings* settings() {
        if (!settings_) {
            if (GDBusConnection* shared = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)) {
                g_dbus_connection_set_exit_on_close(shared, FALSE);
                g_object_unref(shared);
            }
            settings_ = g_settings_new_full(schema_, nullptr, nullptr);
        }
        return settings_;
    }

    uint32_t clamp_temperature(uint32_t kelvin) {
        GSettingsSchemaKey* key = g_settings_schema_get_key(schema_, "night-light-temperature");
        if (!key) return kelvin;
        Var range(g_settings_schema_key_get_range(key));
        g_settings_schema_key_unref(key);
        const gchar* type = nullptr;
        GVariant* detail = nullptr;
        g_variant_get(range.v, "(&sv)", &type, &detail);
        Var hold(detail);
        if (type && std::string(type) == "range" && g_variant_is_of_type(detail, G_VARIANT_TYPE("(uu)"))) {
            guint32 lo = 0, hi = 0;
            g_variant_get(detail, "(uu)", &lo, &hi);
            return std::clamp(kelvin, lo, hi);
        }
        return kelvin;
    }

    GDBusConnection* bus_;
    GSettingsSchema* schema_;
    GSettings* settings_ = nullptr;
    std::optional<Schedule> saved_schedule_;
};

} // namespace

std::unique_ptr<DesktopNightLight> DesktopNightLight::detect() {
    GDBusConnection* bus = open_session_bus();
    if (!bus) return nullptr;

    if (name_has_owner(bus, kKWin) && get_bool(bus, kKWin, kKWinPath, kKWinIface, "available").has_value()) {
        return std::make_unique<KWinNightLight>(bus);
    }
    if (name_has_owner(bus, kGsdColor) || name_has_owner(bus, kMutterConfig)) {
        GSettingsSchemaSource* source = g_settings_schema_source_get_default();
        GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, kColorSchema, TRUE) : nullptr;
        if (schema) return std::make_unique<GnomeNightLight>(bus, schema);
    }
    g_object_unref(bus);
    return nullptr;
}

#endif // BRODISPLAYS_HAVE_GIO

} // namespace brodisplays
