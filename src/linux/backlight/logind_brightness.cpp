#include "logind_brightness.h"

#include <cstdlib>
#include <unistd.h>
#include <vector>

#ifdef BRODISPLAYS_HAVE_GIO
#include <gio/gio.h>
#endif

namespace brodisplays {

#ifndef BRODISPLAYS_HAVE_GIO

struct LogindBrightnessClient::Impl {};

LogindBrightnessClient::LogindBrightnessClient() = default;
LogindBrightnessClient::~LogindBrightnessClient() = default;
LogindBrightnessClient::LogindBrightnessClient(LogindBrightnessClient&&) noexcept = default;
LogindBrightnessClient& LogindBrightnessClient::operator=(LogindBrightnessClient&&) noexcept = default;

bool LogindBrightnessClient::is_available() const noexcept {
    return false;
}

std::string LogindBrightnessClient::resolve_session_path() {
    return "";
}

Result LogindBrightnessClient::set_brightness(std::string_view, uint32_t) {
    return Result::failure("GIO/D-Bus support is not compiled in");
}

#else

namespace {

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

GDBusConnection* open_system_bus() {
    GError* err = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
    if (!address) {
        g_clear_error(&err);
        return nullptr;
    }

    GDBusConnection* conn = g_dbus_connection_new_for_address_sync(
        address,
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &err);
    g_free(address);
    g_clear_error(&err);

    if (conn) {
        g_dbus_connection_set_exit_on_close(conn, FALSE);
    }
    return conn;
}

Var call_dbus(GDBusConnection* conn, const char* dest, const char* path,
              const char* iface, const char* method, GVariant* args,
              const char* reply_type, std::string* out_err = nullptr) {
    if (!conn) {
        if (out_err) *out_err = "No D-Bus connection";
        return Var();
    }

    GError* err = nullptr;
    constexpr int kTimeoutMs = 2000;
    GVariant* res = g_dbus_connection_call_sync(
        conn, dest, path, iface, method, args,
        reply_type ? G_VARIANT_TYPE(reply_type) : nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, kTimeoutMs, nullptr, &err);

    if (err) {
        if (out_err) *out_err = err->message ? err->message : "Unknown D-Bus error";
        g_clear_error(&err);
        return Var();
    }
    return Var(res);
}

} // namespace

struct LogindBrightnessClient::Impl {
    GDBusConnection* conn = nullptr;
    std::string cached_session_path;

    Impl() {
        conn = open_system_bus();
    }

    ~Impl() {
        if (conn) {
            g_object_unref(conn);
            conn = nullptr;
        }
    }
};

LogindBrightnessClient::LogindBrightnessClient() : impl_(std::make_unique<Impl>()) {}
LogindBrightnessClient::~LogindBrightnessClient() = default;
LogindBrightnessClient::LogindBrightnessClient(LogindBrightnessClient&&) noexcept = default;
LogindBrightnessClient& LogindBrightnessClient::operator=(LogindBrightnessClient&&) noexcept = default;

bool LogindBrightnessClient::is_available() const noexcept {
    return impl_ && impl_->conn != nullptr;
}

std::string LogindBrightnessClient::resolve_session_path() {
    if (!impl_ || !impl_->conn) return "";
    if (!impl_->cached_session_path.empty()) {
        return impl_->cached_session_path;
    }

    // 1. Try XDG_SESSION_ID
    const char* xdg_session = std::getenv("XDG_SESSION_ID");
    if (xdg_session && *xdg_session) {
        Var r = call_dbus(impl_->conn, "org.freedesktop.login1", "/org/freedesktop/login1",
                          "org.freedesktop.login1.Manager", "GetSession",
                          g_variant_new("(s)", xdg_session), "(o)");
        if (r) {
            const gchar* obj_path = nullptr;
            g_variant_get(r.v, "(&o)", &obj_path);
            if (obj_path && *obj_path) {
                impl_->cached_session_path = obj_path;
                return impl_->cached_session_path;
            }
        }
    }

    // 2. Try GetSessionByPID(0)
    Var r_pid = call_dbus(impl_->conn, "org.freedesktop.login1", "/org/freedesktop/login1",
                          "org.freedesktop.login1.Manager", "GetSessionByPID",
                          g_variant_new("(u)", static_cast<guint32>(getpid())), "(o)");
    if (r_pid) {
        const gchar* obj_path = nullptr;
        g_variant_get(r_pid.v, "(&o)", &obj_path);
        if (obj_path && *obj_path) {
            impl_->cached_session_path = obj_path;
            return impl_->cached_session_path;
        }
    }

    // 3. Fallback to /org/freedesktop/login1/session/auto
    impl_->cached_session_path = "/org/freedesktop/login1/session/auto";
    return impl_->cached_session_path;
}

Result LogindBrightnessClient::set_brightness(std::string_view device_name, uint32_t raw_brightness) {
    if (!is_available()) {
        return Result::failure("logind system D-Bus connection is not available");
    }

    std::string device_str(device_name);
    std::vector<std::string> session_candidates;

    std::string resolved = resolve_session_path();
    if (!resolved.empty()) {
        session_candidates.push_back(resolved);
    }
    if (resolved != "/org/freedesktop/login1/session/auto") {
        session_candidates.emplace_back("/org/freedesktop/login1/session/auto");
    }
    if (resolved != "/org/freedesktop/login1/session/self") {
        session_candidates.emplace_back("/org/freedesktop/login1/session/self");
    }

    std::string last_error;
    for (const auto& path : session_candidates) {
        std::string err_msg;
        Var res = call_dbus(
            impl_->conn, "org.freedesktop.login1", path.c_str(),
            "org.freedesktop.login1.Session", "SetBrightness",
            g_variant_new("(ssu)", "backlight", device_str.c_str(), static_cast<guint32>(raw_brightness)),
            "()", &err_msg);

        if (res) {
            return Result::success();
        }
        last_error = err_msg;
    }

    // Also attempt Manager.SetBrightness on /org/freedesktop/login1
    std::string mgr_err;
    Var mgr_res = call_dbus(
        impl_->conn, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "SetBrightness",
        g_variant_new("(ssu)", "backlight", device_str.c_str(), static_cast<guint32>(raw_brightness)),
        "()", &mgr_err);

    if (mgr_res) {
        return Result::success();
    }

    return Result::failure("logind SetBrightness failed on all paths: " +
                           (last_error.empty() ? mgr_err : last_error));
}

#endif

} // namespace brodisplays
