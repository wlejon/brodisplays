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

// Drives the "Keep these changes?" window of a temporary configuration.
//
// State machine: idle -> pending -> (confirming -> idle | reverting -> idle).
// A configuration stays *pending* until its rollback has fully run and the
// ConfigurationReverted event is on the queue, so an observer that waits for
// !is_pending() always sees the restored display state and the event. Calls
// that arrive while a rollback or a confirmation is in flight wait for it to
// finish first; the timer does not fire while the persist step runs.
class RevertManager {
public:
    using Rollback = std::function<Result()>;
    using Persist = std::function<Result()>;

    explicit RevertManager(MessageQueue<DisplayEvent>& events);
    ~RevertManager();

    RevertManager(const RevertManager&) = delete;
    RevertManager& operator=(const RevertManager&) = delete;

    // Starts a temporary configuration window with an automatic revert timer.
    // If a previous temporary configuration was already pending, it is reverted first.
    // `persist_action` (optional) is what confirm() runs to make the
    // configuration permanent.
    void start_temporary(
        std::string display_id,
        std::chrono::milliseconds timeout,
        Rollback rollback_action,
        Persist persist_action = nullptr);

    // Runs the persist action and, when it succeeds, ends the window
    // (ConfigurationConfirmed). When it fails the configuration stays
    // pending with its timer still running, and the error is returned.
    Result confirm();

    // Immediately reverts and cancels the timer; returns the rollback's result.
    Result revert();

    // True while an unconfirmed temporary configuration is active, including
    // while its rollback is still being applied.
    bool is_pending() const;

private:
    void timer_loop();
    void wait_for_rollback_locked(std::unique_lock<std::mutex>& lock);
    Result revert_locked(std::unique_lock<std::mutex>& lock, const std::string& reason);

    MessageQueue<DisplayEvent>& events_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ = false;
    bool reverting_ = false;   // a rollback or a persist step is in flight
    std::string pending_display_id_;
    std::chrono::steady_clock::time_point deadline_;
    Rollback rollback_;
    Persist persist_;
    std::thread timer_thread_;
    bool stopping_ = false;
};

} // namespace brodisplays
