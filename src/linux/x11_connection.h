#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <xcb/xcb.h>

namespace brodisplays {

// Abandons a connection whose server stops answering. While armed, a
// deadline is pending; if it passes before disarm(), the watchdog shuts the
// socket down, which makes every blocked or later XCB call on it fail at once
// (XCB then reports a connection error) instead of waiting forever. One
// thread per connection, idle while disarmed.
class X11Watchdog {
public:
    X11Watchdog();
    ~X11Watchdog();
    X11Watchdog(const X11Watchdog&) = delete;
    X11Watchdog& operator=(const X11Watchdog&) = delete;

    void set_fd(int fd);
    void arm(std::chrono::milliseconds timeout);
    void disarm();
    bool fired() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int fd_ = -1;
    bool armed_ = false;
    bool fired_ = false;
    bool stopping_ = false;
    std::chrono::steady_clock::time_point deadline_;
    std::thread thread_;
};

// An XCB connection to the X server $DISPLAY names, where every exchange is
// bounded. Local displays are connected by hand (a non-blocking connect to
// the abstract or filesystem socket, Xauthority credentials, then
// xcb_connect_to_fd with the watchdog armed for the setup handshake), so even
// a server that accepts the connection and never answers costs at most the
// timeout. A TCP display is connected with xcb_connect on a helper thread
// that is abandoned if it does not return in time.
class X11Connection {
public:
    static constexpr std::chrono::milliseconds kIoTimeout{2000};

    // nullptr (and *error) when there is no usable server.
    static std::unique_ptr<X11Connection> open(std::chrono::milliseconds timeout, std::string* error);
    ~X11Connection();

    X11Connection(const X11Connection&) = delete;
    X11Connection& operator=(const X11Connection&) = delete;

    xcb_connection_t* get() const { return conn_; }
    xcb_window_t root() const { return root_; }
    const xcb_screen_t* screen() const { return screen_; }
    int fd() const { return fd_; }
    // The connection failed (server gone, protocol error, or abandoned).
    bool broken() const;
    bool abandoned() const { return watchdog_.fired(); }

    // Arms the watchdog for one bounded operation (any number of requests).
    class Op {
    public:
        explicit Op(X11Connection& c, std::chrono::milliseconds timeout = kIoTimeout) : c_(c) {
            c_.watchdog_.arm(timeout);
        }
        ~Op() { c_.watchdog_.disarm(); }
        Op(const Op&) = delete;
        Op& operator=(const Op&) = delete;

    private:
        X11Connection& c_;
    };

private:
    X11Connection() = default;

    xcb_connection_t* conn_ = nullptr;
    xcb_window_t root_ = 0;
    const xcb_screen_t* screen_ = nullptr;
    int fd_ = -1;
    X11Watchdog watchdog_;
};

} // namespace brodisplays
