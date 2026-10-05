#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "brodisplays/event_queue.h"
#include "brodisplays/types.h"

namespace brodisplays {

class RevertManager {
public:
    explicit RevertManager(MessageQueue<DisplayEvent>& events);
    ~RevertManager();

    RevertManager(const RevertManager&) = delete;
    RevertManager& operator=(const RevertManager&) = delete;

    // Starts a temporary configuration window with an automatic revert timer.
    // If a previous temporary configuration was already pending, it is reverted first.
    void start_temporary(
        std::string display_id,
        std::chrono::milliseconds timeout,
        std::function<void()> rollback_action);

    // Confirms and cancels the revert timer.
    Result confirm();

    // Immediately reverts and cancels the timer.
    Result revert();

    // Checks whether an unconfirmed temporary configuration is active.
    bool is_pending() const;

private:
    void timer_loop();
    void cancel_and_revert_locked(std::unique_lock<std::mutex>& lock, const std::string& reason);

    MessageQueue<DisplayEvent>& events_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ = false;
    std::string pending_display_id_;
    std::chrono::steady_clock::time_point deadline_;
    std::function<void()> rollback_;
    std::thread timer_thread_;
    bool stopping_ = false;
};

} // namespace brodisplays
