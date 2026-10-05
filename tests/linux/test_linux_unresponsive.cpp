#include "check.h"
#include "brodisplays/display_service.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// Regression: a display server that accepts connections but never answers
// (wedged, or still starting) must not hang DisplayService. Both a fake
// Wayland socket and a fake X11 display are listening sockets nobody accepts
// on: connect() succeeds through the backlog, every read would block forever.

namespace {

int listen_unix(const std::string& path, bool abstract) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    socklen_t len;
    if (abstract) {
        std::memcpy(addr.sun_path + 1, path.data(), path.size());
        len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + path.size());
    } else {
        unlink(path.c_str());
        std::memcpy(addr.sun_path, path.data(), path.size());
        len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main() {
    std::printf("[test_linux_unresponsive] DisplayService against servers that never answer\n");

    char dir_tmpl[] = "/tmp/brodisplays-unresp-XXXXXX";
    char* dir = mkdtemp(dir_tmpl);
    REQUIRE(dir != nullptr);
    chmod(dir, 0700);

    const std::string wl_name = "wl-silent";
    int wl_fd = listen_unix(std::string(dir) + "/" + wl_name, false);
    REQUIRE(wl_fd >= 0);
    // X display :<n> is looked up as the abstract socket @/tmp/.X11-unix/X<n>
    // first, which needs no write access to /tmp/.X11-unix.
    std::string x_num;
    int x_fd = -1;
    for (int n = 937; n < 960 && x_fd < 0; ++n) {
        x_fd = listen_unix("/tmp/.X11-unix/X" + std::to_string(n), true);
        if (x_fd >= 0) x_num = std::to_string(n);
    }
    REQUIRE(x_fd >= 0);

    setenv("XDG_RUNTIME_DIR", dir, 1);
    setenv("WAYLAND_DISPLAY", wl_name.c_str(), 1);
    setenv("DISPLAY", (":" + x_num).c_str(), 1);

    const auto t0 = std::chrono::steady_clock::now();
    {
        std::string err;
        auto service = brodisplays::DisplayService::create({}, &err);
        REQUIRE(service != nullptr);
        auto snap = service->snapshot();
        std::printf("Enumerated %zu display(s) via fallback\n", snap.displays.size());
        for (const auto& d : snap.displays) {
            // Only the kernel's view can answer here.
            CHECK_EQ(d.adapter_name, std::string("DRM Kernel Device"));
        }
        brodisplays::DisplayConfigChange change;
        change.width = 1024;
        change.height = 768;
        auto res = service->apply_configuration(change);
        CHECK(!res.ok);
        std::printf("apply_configuration: %s\n", res.error.c_str());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    std::printf("Create/query/apply/destroy took %lld ms\n", static_cast<long long>(elapsed.count()));
    CHECK(elapsed < std::chrono::seconds(10));

    close(wl_fd);
    close(x_fd);
    unlink((std::string(dir) + "/" + wl_name).c_str());
    rmdir(dir);
    return bstest::finish("test_linux_unresponsive");
}
