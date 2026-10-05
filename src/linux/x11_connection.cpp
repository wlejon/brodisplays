#include "x11_connection.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <X11/Xauth.h>

namespace brodisplays {

// ---------------------------------------------------------------- watchdog

X11Watchdog::X11Watchdog() : thread_(&X11Watchdog::run, this) {}

X11Watchdog::~X11Watchdog() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void X11Watchdog::set_fd(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    fd_ = fd;
}

void X11Watchdog::arm(std::chrono::milliseconds timeout) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        armed_ = true;
        deadline_ = std::chrono::steady_clock::now() + timeout;
    }
    cv_.notify_all();
}

void X11Watchdog::disarm() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = false;
}

bool X11Watchdog::fired() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fired_;
}

void X11Watchdog::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (!armed_) {
            cv_.wait(lock, [&] { return stopping_ || armed_; });
            continue;
        }
        const auto deadline = deadline_;
        if (cv_.wait_until(lock, deadline, [&] { return stopping_ || !armed_ || deadline_ != deadline; })) continue;
        // Still armed with the same deadline, which has passed.
        if (fd_ >= 0 && !fired_) shutdown(fd_, SHUT_RDWR);
        fired_ = true;
        armed_ = false;
    }
}

// ---------------------------------------------------------------- connection

namespace {

struct DisplayName {
    bool local = true;
    std::string socket_path;  // filesystem socket (launchd-style DISPLAY), else empty
    std::string number;
    int screen = 0;
};

// $DISPLAY: [host|unix|/socket/path]:number[.screen]
bool parse_display(const char* env, DisplayName& out) {
    if (!env || !*env) return false;
    std::string d = env;
    size_t colon = d.rfind(':');
    if (colon == std::string::npos) return false;
    std::string host = d.substr(0, colon);
    std::string rest = d.substr(colon + 1);
    size_t dot = rest.find('.');
    out.number = rest.substr(0, dot);
    if (out.number.empty() || out.number.find_first_not_of("0123456789") != std::string::npos) return false;
    if (dot != std::string::npos) out.screen = std::atoi(rest.c_str() + dot + 1);
    if (!host.empty() && host[0] == '/') {
        out.socket_path = host;
        out.local = true;
    } else {
        out.local = host.empty() || host == "unix";
    }
    return true;
}

// Non-blocking connect to one local socket; -1 when nothing listens there.
int connect_unix(const std::string& path, bool abstract, std::chrono::steady_clock::time_point deadline) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 2 > sizeof(addr.sun_path)) return -1;
    socklen_t len;
    if (abstract) {
        std::memcpy(addr.sun_path + 1, path.data(), path.size());
        len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + path.size());
    } else {
        std::memcpy(addr.sun_path, path.data(), path.size());
        len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    }
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0) return fd;
        int e = errno;
        close(fd);
        // A full listen backlog (EAGAIN) means a server that is not
        // accepting; retry until the deadline.
        if (e != EAGAIN || std::chrono::steady_clock::now() >= deadline) return -1;
        usleep(10000);
    }
}

std::unique_ptr<X11Connection> fail(std::string* error, std::string why) {
    if (error) *error = std::move(why);
    return nullptr;
}

const xcb_screen_t* screen_of(xcb_connection_t* c, int index) {
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; it.rem; ++i, xcb_screen_next(&it)) {
        if (i == index) return it.data;
    }
    return nullptr;
}

} // namespace

std::unique_ptr<X11Connection> X11Connection::open(std::chrono::milliseconds timeout, std::string* error) {
    DisplayName name;
    if (!parse_display(std::getenv("DISPLAY"), name)) return fail(error, "DISPLAY is not set");

    std::unique_ptr<X11Connection> self(new X11Connection());
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    if (name.local) {
        int fd = -1;
        if (!name.socket_path.empty()) {
            fd = connect_unix(name.socket_path + ":" + name.number, false, deadline);
        } else {
            const std::string path = "/tmp/.X11-unix/X" + name.number;
            fd = connect_unix(path, true, deadline);
            if (fd < 0) fd = connect_unix(path, false, deadline);
        }
        if (fd < 0) return fail(error, "no X server listens on display :" + name.number);

        // Credentials, as xcb_connect looks them up for a local connection.
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname) - 1);
        char auth_name[] = "MIT-MAGIC-COOKIE-1";
        char* names[] = {auth_name};
        int lens[] = {static_cast<int>(std::strlen(auth_name))};
        Xauth* auth = XauGetBestAuthByAddr(FamilyLocal, static_cast<unsigned short>(std::strlen(hostname)), hostname,
                                           static_cast<unsigned short>(name.number.size()), name.number.c_str(), 1,
                                           names, lens);
        xcb_auth_info_t info{};
        if (auth) {
            info.namelen = auth->name_length;
            info.name = auth->name;
            info.datalen = auth->data_length;
            info.data = auth->data;
        }

        // The setup handshake blocks inside XCB; the watchdog bounds it.
        self->fd_ = fd;
        self->watchdog_.set_fd(fd);
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        self->watchdog_.arm(std::max(left, std::chrono::milliseconds(1)));
        self->conn_ = xcb_connect_to_fd(fd, auth ? &info : nullptr);
        self->watchdog_.disarm();
        if (auth) XauDisposeAuth(auth);
    } else {
        // TCP: xcb_connect on a helper thread. If it does not come back in
        // time it is abandoned; should it finish later it disconnects itself.
        struct Shared {
            std::mutex m;
            bool abandoned = false;
            xcb_connection_t* conn = nullptr;
            int screen = 0;
        };
        auto shared = std::make_shared<Shared>();
        auto done = std::make_shared<std::promise<void>>();
        auto ready = done->get_future();
        std::thread([shared, done] {
            int screen = 0;
            xcb_connection_t* c = xcb_connect(nullptr, &screen);
            std::lock_guard<std::mutex> lock(shared->m);
            if (shared->abandoned) {
                xcb_disconnect(c);
                return;
            }
            shared->conn = c;
            shared->screen = screen;
            done->set_value();
        }).detach();
        if (ready.wait_until(deadline) != std::future_status::ready) {
            std::lock_guard<std::mutex> lock(shared->m);
            if (!shared->conn) {
                shared->abandoned = true;
                return fail(error, "the X server did not complete the connection setup in time");
            }
        }
        self->conn_ = shared->conn;
        name.screen = shared->screen;
        self->fd_ = xcb_get_file_descriptor(self->conn_);
        self->watchdog_.set_fd(self->fd_);
    }

    if (!self->conn_ || xcb_connection_has_error(self->conn_)) {
        std::string why = self->watchdog_.fired() ? "the X server did not answer the connection setup in time"
                                                  : "the X server refused the connection";
        if (self->conn_) xcb_disconnect(self->conn_);
        self->conn_ = nullptr;
        return fail(error, why);
    }
    self->screen_ = screen_of(self->conn_, name.screen);
    if (!self->screen_) return fail(error, "the X display has no screen " + std::to_string(name.screen));
    self->root_ = self->screen_->root;
    return self;
}

X11Connection::~X11Connection() {
    // xcb_disconnect only shuts the socket down and frees; it never waits
    // for the server.
    if (conn_) xcb_disconnect(conn_);
}

bool X11Connection::broken() const {
    return !conn_ || xcb_connection_has_error(conn_) != 0 || watchdog_.fired();
}

} // namespace brodisplays
