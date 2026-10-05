#pragma once

// Private display servers and session buses for the Linux tests: a headless
// sway, an Xvfb, a virtual KWin, a headless mutter, each in its own runtime
// directory, config home and D-Bus session, so a test never touches the
// user's session. Everything a test starts runs in its own process group and
// is killed (group-wide, so D-Bus-activated helpers go too) when the owning
// object is destroyed.

#include <chrono>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace bdtest {

bool have_program(const std::string& name);
// Runs a command with /bin/sh, returns its stdout (bounded by `timeout`
// through coreutils timeout(1)).
std::string run(const std::string& command, int timeout_seconds = 5);

class Process {
public:
    Process() = default;
    // Starts argv[0] (searched in PATH) in a new session; stdout/stderr go
    // to `log`.
    Process(const std::vector<std::string>& argv, const std::string& log);
    ~Process();
    Process(Process&& o) noexcept;
    Process& operator=(Process&& o) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    pid_t pid() const { return pid_; }
    bool running() const;
    void stop();  // SIGCONT + SIGTERM the group, then SIGKILL after 2 s

private:
    pid_t pid_ = -1;
};

// A fresh 0700 directory under /tmp, removed (recursively) on destruction.
class TempDir {
public:
    explicit TempDir(const char* tag);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds timeout);

std::string read_file(const std::string& path);
bool write_file(const std::string& path, const std::string& content);

// Sets up the environment of an isolated session for this process (and what
// it starts): XDG_RUNTIME_DIR and XDG_CONFIG_HOME in `dir`, no DISPLAY,
// WAYLAND_DISPLAY, XDG_CURRENT_DESKTOP, and a D-Bus address that leads
// nowhere until start_bus() replaces it.
void isolate_environment(const TempDir& dir);

// A private dbus-daemon; sets DBUS_SESSION_BUS_ADDRESS. Empty pid on failure.
Process start_bus(const TempDir& dir);

// Xvfb on a free display number; sets DISPLAY. Not running on failure.
Process start_xvfb(const TempDir& dir, int width, int height);

// Xorg with the dummy video driver (an X server whose CRTCs keep real gamma
// ramps, unlike Xvfb's), run as the user with no VT; sets DISPLAY. Not
// running when Xorg or the dummy driver is missing.
Process start_xorg_dummy(const TempDir& dir);

// A compositor whose Wayland socket is `socket` in XDG_RUNTIME_DIR; waits
// for the socket and sets WAYLAND_DISPLAY. Not running on failure.
Process start_wayland(const TempDir& dir, const std::vector<std::string>& argv, const std::string& socket,
                      const std::string& log_name);

} // namespace bdtest
