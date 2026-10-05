#include "revert_timer.h"

#include <algorithm>

namespace brodisplays {

RevertManager::RevertManager(MessageQueue<DisplayEvent>& events)
    : events_(events) {
    timer_thread_ = std::thread(&RevertManager::timer_loop, this);
}

RevertManager::~RevertManager() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
        cv_.notify_all();
        wait_for_rollback_locked(lock);
        if (pending_) {
            revert_locked(lock, "Service destroyed while configuration unconfirmed");
        }
    }
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

void RevertManager::wait_for_rollback_locked(std::unique_lock<std::mutex>& lock) {
    cv_.wait(lock, [&] { return !reverting_; });
}

void RevertManager::start_temporary(
    std::string display_id,
    std::chrono::milliseconds timeout,
    Rollback rollback_action,
    Persist persist_action) {
    std::unique_lock<std::mutex> lock(mutex_);
    wait_for_rollback_locked(lock);
    if (pending_) {
        // Revert previous before starting new
        revert_locked(lock, "Superseded by new configuration change");
        wait_for_rollback_locked(lock);
    }

    pending_ = true;
    pending_display_id_ = std::move(display_id);
    deadline_ = std::chrono::steady_clock::now() + timeout;
    rollback_ = std::move(rollback_action);
    persist_ = std::move(persist_action);
    cv_.notify_all();
}

Result RevertManager::confirm() {
    std::unique_lock<std::mutex> lock(mutex_);
    wait_for_rollback_locked(lock);
    if (!pending_) {
        return Result::failure("No configuration change is currently pending");
    }
    std::string confirmed_id = pending_display_id_;
    if (persist_) {
        // The persist step runs unlocked; reverting_ keeps the timer and any
        // concurrent confirm/revert out until it has finished.
        Persist persist = persist_;
        reverting_ = true;
        cv_.notify_all();
        lock.unlock();
        Result persisted = persist();
        lock.lock();
        reverting_ = false;
        cv_.notify_all();
        if (!persisted) {
            return Result::failure("The configuration could not be persisted: " + persisted.error);
        }
    }
    pending_ = false;
    rollback_ = nullptr;
    persist_ = nullptr;
    cv_.notify_all();
    lock.unlock();

    events_.push(ConfigurationConfirmed{confirmed_id});
    return Result::success();
}

Result RevertManager::revert() {
    std::unique_lock<std::mutex> lock(mutex_);
    wait_for_rollback_locked(lock);
    if (!pending_) {
        return Result::failure("No configuration change is currently pending");
    }
    return revert_locked(lock, "User reverted configuration");
}

bool RevertManager::is_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

// Runs the rollback with the lock released. pending_ stays true and reverting_
// marks the in-flight rollback until the display is restored and the event is
// published; only then does the configuration count as no longer pending.
Result RevertManager::revert_locked(std::unique_lock<std::mutex>& lock, const std::string& reason) {
    if (!pending_ || reverting_) {
        return Result::failure("No configuration change is currently pending");
    }
    auto rollback = std::move(rollback_);
    rollback_ = nullptr;
    persist_ = nullptr;
    std::string display_id = pending_display_id_;
    reverting_ = true;
    cv_.notify_all();

    lock.unlock();
    Result result = rollback ? rollback() : Result::failure("No rollback action registered");
    events_.push(ConfigurationReverted{display_id, reason, result.ok, result.error});
    lock.lock();

    reverting_ = false;
    pending_ = false;
    cv_.notify_all();
    return result;
}

void RevertManager::timer_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (!pending_ || reverting_) {
            cv_.wait(lock, [&] { return stopping_ || (pending_ && !reverting_); });
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline_) {
            revert_locked(lock, "Confirmation timeout expired");
            continue;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
        auto step = std::min(remaining, std::chrono::milliseconds(500));

        // Report countdown
        std::string cur_id = pending_display_id_;
        auto cur_deadline = deadline_;
        lock.unlock();
        events_.push(RevertCountdown{cur_id, remaining});
        lock.lock();

        // Wake early when the window is confirmed, reverted, replaced or the
        // manager is shutting down; otherwise loop to re-check the deadline.
        cv_.wait_for(lock, step, [&] {
            return stopping_ || !pending_ || reverting_ || deadline_ != cur_deadline;
        });
    }
}

} // namespace brodisplays
