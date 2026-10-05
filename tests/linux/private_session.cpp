#include "private_session.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace bdtest {

namespace fs = std::filesystem;

namespace {

// Built by appending: GCC 12 warns falsely (-Wrestrict, PR 105651) on
// "literal" + std::string temporaries.
std::string join(std::initializer_list<std::string_view> parts) {
    std::string s;
    for (auto p : parts) s.append(p);
    return s;
}

std::string x_display(int n) {
    return join({":", std::to_string(n)});
}

int free_x_display() {
    for (int n = 171; n < 240; ++n) {
        const std::string sock = join({"/tmp/.X11-unix/X", std::to_string(n)});
        const std::string lock = join({"/tmp/.X", std::to_string(n), "-lock"});
        if (access(sock.c_str(), F_OK) != 0 && access(lock.c_str(), F_OK) != 0) return n;
    }
    return -1;
}

} // namespace

bool have_program(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string dir = p.substr(start, end - start);
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return access(name.c_str(), X_OK) == 0;
}

std::string run(const std::string& command, int timeout_seconds) {
    // The command reaches sh through the environment, so it needs no quoting.
    setenv("BDTEST_COMMAND", command.c_str(), 1);
    const std::string wrapped =
        join({"timeout -s KILL ", std::to_string(timeout_seconds), " sh -c \"$BDTEST_COMMAND\" 2>/dev/null"});
    std::string out;
    if (FILE* f = popen(wrapped.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        pclose(f);
    }
    unsetenv("BDTEST_COMMAND");
    return out;
}

Process::Process(const std::vector<std::string>& argv, const std::string& log) {
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int in = open("/dev/null", O_RDONLY);
        int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) {
            dup2(out, 1);
            dup2(out, 2);
        }
        execvp(args[0], args.data());
        _exit(127);
    }
    pid_ = pid;
}

Process::~Process() {
    stop();
}

Process::Process(Process&& o) noexcept : pid_(o.pid_) {
    o.pid_ = -1;
}

Process& Process::operator=(Process&& o) noexcept {
    if (this != &o) {
        stop();
        pid_ = o.pid_;
        o.pid_ = -1;
    }
    return *this;
}

bool Process::running() const {
    if (pid_ <= 0) return false;
    int status = 0;
    return waitpid(pid_, &status, WNOHANG) == 0;
}

void Process::stop() {
    if (pid_ <= 0) return;
    // The whole group: what the process started (D-Bus-activated helpers,
    // Xwayland) goes with it.
    kill(-pid_, SIGCONT);
    kill(-pid_, SIGTERM);
    int status = 0;
    bool reaped = wait_for([&] { return waitpid(pid_, &status, WNOHANG) != 0; }, std::chrono::milliseconds(2000));
    kill(-pid_, SIGKILL);
    if (!reaped) waitpid(pid_, &status, 0);
    pid_ = -1;
}

TempDir::TempDir(const char* tag) {
    std::string tmpl = join({"/tmp/brodisplays-", tag, "-XXXXXX"});
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data())) {
        path_ = buf.data();
        chmod(path_.c_str(), 0700);
    }
}

TempDir::~TempDir() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
}

bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void isolate_environment(const TempDir& dir) {
    const std::string base = dir.path();
    for (const char* sub : {"/run", "/config", "/data", "/cache", "/state"}) {
        mkdir((base + sub).c_str(), 0700);
    }
    setenv("XDG_RUNTIME_DIR", (base + "/run").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (base + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
    setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
    for (const char* v : {"DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
                          "XDG_SESSION_DESKTOP", "DESKTOP_SESSION", "SWAYSOCK", "XAUTHORITY"}) {
        unsetenv(v);
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", join({"unix:path=", base, "/no-bus"}).c_str(), 1);
}

Process start_bus(const TempDir& dir) {
    if (!have_program("dbus-daemon")) return {};
    const std::string socket = dir.path() + "/bus";
    Process p({"dbus-daemon", "--session", "--nofork", "--nopidfile", join({"--address=unix:path=", socket})},
              dir.path() + "/dbus.log");
    if (!wait_for([&] { return access(socket.c_str(), F_OK) == 0; }, std::chrono::milliseconds(5000))) {
        return {};
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", join({"unix:path=", socket}).c_str(), 1);
    return p;
}

// Starts an X server on the display number `argv` names (":N" filled in as
// argv[1]) and waits for its socket; sets DISPLAY.
static Process start_x_server(std::vector<std::string> argv, const std::string& log) {
    const int n = free_x_display();
    if (n < 0) return {};
    argv.insert(argv.begin() + 1, x_display(n));
    const std::string sock = join({"/tmp/.X11-unix/X", std::to_string(n)});
    Process p(argv, log);
    if (wait_for([&] { return access(sock.c_str(), F_OK) == 0 || !p.running(); }, std::chrono::milliseconds(10000)) &&
        p.running()) {
        setenv("DISPLAY", x_display(n).c_str(), 1);
        return p;
    }
    return {};
}

Process start_xvfb(const TempDir& dir, int width, int height) {
    if (!have_program("Xvfb")) return {};
    const std::string screen = join({std::to_string(width), "x", std::to_string(height), "x24"});
    return start_x_server({"Xvfb", "-screen", "0", screen, "-nolisten", "tcp", "-noreset"}, dir.path() + "/xvfb.log");
}

namespace {

const char* kDummyConfig = R"(Section "ServerFlags"
    Option "AutoAddDevices" "false"
    Option "AutoEnableDevices" "false"
EndSection
Section "Monitor"
    Identifier "Monitor0"
    HorizSync 5.0 - 1000.0
    VertRefresh 5.0 - 200.0
    Modeline "1920x1080_60" 173.00 1920 2048 2248 2576 1080 1083 1088 1120 -hsync +vsync
    Modeline "1280x1024_60" 109.00 1280 1368 1496 1712 1024 1027 1034 1063 -hsync +vsync
EndSection
Section "Device"
    Identifier "Card0"
    Driver "dummy"
    VideoRam 256000
EndSection
Section "Screen"
    Identifier "Screen0"
    Device "Card0"
    Monitor "Monitor0"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "1920x1080_60" "1280x1024_60"
        Virtual 1920 1080
    EndSubSection
EndSection
)";

} // namespace

Process start_xorg_dummy(const TempDir& dir) {
    const char* server = "/usr/lib/Xorg";  // the server itself, not the setuid wrapper
    if (access(server, X_OK) != 0 || access("/usr/lib/xorg/modules/drivers/dummy_drv.so", R_OK) != 0) return {};
    const std::string conf_dir = dir.path() + "/xorg.conf.d";
    mkdir(conf_dir.c_str(), 0700);
    const std::string conf = dir.path() + "/xorg-dummy.conf";
    if (!write_file(conf, kDummyConfig)) return {};
    return start_x_server({server, "-config", conf, "-configdir", conf_dir, "-logfile", dir.path() + "/xorg.log",
                           "-nolisten", "tcp", "-noreset"},
                          dir.path() + "/xorg.out");
}

Process start_wayland(const TempDir& dir, const std::vector<std::string>& argv, const std::string& socket,
                      const std::string& log_name) {
    if (argv.empty() || !have_program(argv[0])) return {};
    const std::string run_dir = dir.path() + "/run";
    Process p(argv, dir.path() + "/" + log_name);
    std::string found;
    auto ready = [&] {
        if (!p.running()) return true;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(run_dir, ec)) {
            const std::string name = e.path().filename().string();
            if (name.size() > 5 && name.compare(name.size() - 5, 5, ".lock") == 0) continue;
            if (socket.empty() ? name.rfind("wayland-", 0) == 0 : name == socket) {
                found = name;
                return true;
            }
        }
        return false;
    };
    if (!wait_for(ready, std::chrono::milliseconds(15000)) || found.empty()) return {};
    setenv("WAYLAND_DISPLAY", found.c_str(), 1);
    return p;
}

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

bool write_file(const std::string& path, const std::string& content) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace bdtest
