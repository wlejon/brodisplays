#include "linux_backlight_watcher.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "sysfs_backlight.h"

namespace brodisplays {

namespace {

struct WatchedDeviceState {
    BacklightDevice device;
    int inotify_wd = -1;
    int actual_brightness_fd = -1;
    uint32_t last_brightness = 0;
};

void safe_close(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace

LinuxBacklightWatcher::LinuxBacklightWatcher(
    std::string target_device_or_connector,
    std::function<void(const BrightnessEvent&)> on_event)
    : target_(std::move(target_device_or_connector)),
      on_event_(std::move(on_event)) {
    stop_event_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_event_fd_ >= 0) {
        running_ = true;
        thread_ = std::thread(&LinuxBacklightWatcher::run_loop, this);
    }
}

LinuxBacklightWatcher::~LinuxBacklightWatcher() {
    stop();
}

bool LinuxBacklightWatcher::is_running() const noexcept {
    return running_.load();
}

void LinuxBacklightWatcher::stop() {
    if (running_.exchange(false)) {
        if (stop_event_fd_ >= 0) {
            uint64_t val = 1;
            (void)::write(stop_event_fd_, &val, sizeof(val));
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        safe_close(stop_event_fd_);
    }
}

void LinuxBacklightWatcher::run_loop() {
    int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        running_ = false;
        return;
    }

    int inotify_fd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);

    // Register stop event fd
    if (stop_event_fd_ >= 0) {
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = stop_event_fd_;
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, stop_event_fd_, &ev);
    }

    // Register inotify fd
    if (inotify_fd >= 0) {
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = inotify_fd;
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, inotify_fd, &ev);
    }

    // Identify target devices to watch
    std::vector<BacklightDevice> target_devices;
    auto all_devices = enumerate_sysfs_backlight_devices();

    if (target_.empty()) {
        target_devices = std::move(all_devices);
    } else {
        for (auto& dev : all_devices) {
            if (dev.name == target_ || dev.associated_connector == target_) {
                target_devices.push_back(std::move(dev));
                break;
            }
        }
    }

    std::unordered_map<std::string, WatchedDeviceState> states;

    for (auto& dev : target_devices) {
        WatchedDeviceState st;
        st.device = dev;
        st.last_brightness = dev.current_brightness;

        // Inotify watch on device brightness file
        if (inotify_fd >= 0) {
            std::string br_file = dev.sysfs_path + "/brightness";
            st.inotify_wd = ::inotify_add_watch(inotify_fd, br_file.c_str(),
                                                IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
            if (st.inotify_wd < 0) {
                // If brightness file watch fails, try watching the directory
                st.inotify_wd = ::inotify_add_watch(inotify_fd, dev.sysfs_path.c_str(),
                                                    IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
            }
        }

        // Open actual_brightness with POLLPRI for sysfs_notify
        std::string actual_file = dev.sysfs_path + "/actual_brightness";
        st.actual_brightness_fd = ::open(actual_file.c_str(), O_RDONLY | O_CLOEXEC);
        if (st.actual_brightness_fd >= 0) {
            struct epoll_event ev{};
            ev.events = EPOLLPRI | EPOLLERR;
            ev.data.fd = st.actual_brightness_fd;
            ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, st.actual_brightness_fd, &ev);
        }

        states.emplace(dev.name, std::move(st));
    }

    constexpr int kMaxEvents = 16;
    struct epoll_event events[kMaxEvents];

    while (running_.load()) {
        int nfds = ::epoll_wait(epoll_fd, events, kMaxEvents, 500);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        bool should_exit = false;
        bool check_devices = (nfds == 0); // Periodic fallback check

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == stop_event_fd_) {
                should_exit = true;
                break;
            }

            if (fd == inotify_fd) {
                alignas(struct inotify_event) char buf[4096];
                while (::read(inotify_fd, buf, sizeof(buf)) > 0) {}
                check_devices = true;
            } else {
                // One of actual_brightness fds
                for (auto& [name, st] : states) {
                    if (st.actual_brightness_fd == fd) {
                        char dump[64];
                        ::lseek(st.actual_brightness_fd, 0, SEEK_SET);
                        (void)::read(st.actual_brightness_fd, dump, sizeof(dump));
                        check_devices = true;
                        break;
                    }
                }
            }
        }

        if (should_exit) break;

        if (check_devices) {
            for (auto& [name, st] : states) {
                auto updated = read_sysfs_backlight_device(name);
                if (updated && updated->current_brightness != st.last_brightness) {
                    st.last_brightness = updated->current_brightness;
                    st.device = *updated;

                    if (on_event_) {
                        BrightnessEvent ev;
                        ev.device_name = updated->name;
                        ev.connector_name = updated->associated_connector;
                        ev.raw_brightness = updated->current_brightness;
                        ev.max_brightness = updated->max_brightness;
                        ev.min_brightness = updated->min_brightness;
                        ev.normalized_brightness = updated->normalized_brightness;
                        ev.timestamp = std::chrono::system_clock::now();
                        on_event_(ev);
                    }
                }
            }
        }
    }

    // Cleanup
    for (auto& [name, st] : states) {
        if (inotify_fd >= 0 && st.inotify_wd >= 0) {
            ::inotify_rm_watch(inotify_fd, st.inotify_wd);
        }
        if (st.actual_brightness_fd >= 0) {
            ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, st.actual_brightness_fd, nullptr);
            safe_close(st.actual_brightness_fd);
        }
    }

    safe_close(inotify_fd);
    safe_close(epoll_fd);
}

} // namespace brodisplays
