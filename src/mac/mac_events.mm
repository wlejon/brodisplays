#import "mac_events.h"

#import <CoreGraphics/CoreGraphics.h>

#include <vector>

namespace brodisplays {

MacDisplayWatcher::MacDisplayWatcher(std::function<void()> on_change)
    : on_change_(std::move(on_change)) {
    CGDisplayRegisterReconfigurationCallback(display_reconfig_callback, this);
    poller_ = std::thread(&MacDisplayWatcher::poll_loop, this);
}

MacDisplayWatcher::~MacDisplayWatcher() {
    CGDisplayRemoveReconfigurationCallback(display_reconfig_callback, this);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (poller_.joinable()) poller_.join();
}

std::string MacDisplayWatcher::fingerprint() {
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess) return "error";
    std::vector<CGDirectDisplayID> ids(count);
    if (count > 0 && CGGetOnlineDisplayList(count, ids.data(), &count) != kCGErrorSuccess) return "error";
    std::string out;
    for (uint32_t i = 0; i < count; ++i) {
        CGDirectDisplayID d = ids[i];
        CGRect b = CGDisplayBounds(d);
        out += std::to_string(d) + ":" + std::to_string(b.origin.x) + "," + std::to_string(b.origin.y) + "," +
               std::to_string(b.size.width) + "x" + std::to_string(b.size.height);
        if (CGDisplayModeRef m = CGDisplayCopyDisplayMode(d)) {
            out += "/" + std::to_string(CGDisplayModeGetPixelWidth(m)) + "x" +
                   std::to_string(CGDisplayModeGetPixelHeight(m)) + "@" +
                   std::to_string(CGDisplayModeGetRefreshRate(m)) + "#" +
                   std::to_string(CGDisplayModeGetIODisplayModeID(m));
            CGDisplayModeRelease(m);
        }
        out += "/r" + std::to_string(CGDisplayRotation(d));
        out += CGDisplayIsAsleep(d) ? "/asleep" : "/awake";
        out += CGDisplayIsMain(d) ? "/main" : "";
        out += "/m" + std::to_string(CGDisplayMirrorsDisplay(d)) + ";";
    }
    return out;
}

void MacDisplayWatcher::poll_loop() {
    std::string last = fingerprint();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, kPollInterval, [&] { return stopping_; });
        if (stopping_) break;
        lock.unlock();
        std::string now = fingerprint();
        if (now != last) {
            last = now;
            if (on_change_) on_change_();
        }
        lock.lock();
    }
}

void MacDisplayWatcher::display_reconfig_callback(
    uint32_t /*display*/,
    uint32_t flags,
    void* user_info) {
    if (!user_info) return;

    // Trigger only on completion of reconfiguration
    if ((flags & kCGDisplayBeginConfigurationFlag) == 0) {
        auto* watcher = static_cast<MacDisplayWatcher*>(user_info);
        if (watcher->on_change_) {
            watcher->on_change_();
        }
    }
}

} // namespace brodisplays
