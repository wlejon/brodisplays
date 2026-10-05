#import "mac_events.h"

#import <CoreGraphics/CoreGraphics.h>

namespace brodisplays {

MacDisplayWatcher::MacDisplayWatcher(std::function<void()> on_change)
    : on_change_(std::move(on_change)) {
    CGDisplayRegisterReconfigurationCallback(display_reconfig_callback, this);
}

MacDisplayWatcher::~MacDisplayWatcher() {
    CGDisplayRemoveReconfigurationCallback(display_reconfig_callback, this);
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
