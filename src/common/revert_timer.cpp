#include "revert_timer.h"

namespace brodisplays {

RevertManager::RevertManager(MessageQueue<DisplayEvent>& events)
    : events_(events) {
    timer_thread_ = std::thread(&RevertManager::timer_loop, this);
}

RevertManager::~RevertManager() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
        if (pending_ && rollback_) {
            auto rollback = std::move(rollback_);
            auto display_id = pending_display_id_;
            pending_ = false;
            rollback_ = nullptr;
            lock.unlock();
            rollback();
            events_.push(ConfigurationReverted{display_id, "Service destroyed while configuration unconfirmed"});
            lock.lock();
        }
        cv_.notify_all();
    }
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

void RevertManager::start_temporary(
    std::string display_id,
    std::chrono::milliseconds timeout,
    std::function<void()> rollback_action) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (pending_ && rollback_) {
        // Revert previous before starting new
        cancel_and_revert_locked(lock, "Superseded by new configuration change");
    }

    pending_ = true;
    pending_display_id_ = std::move(display_id);
    deadline_ = std::chrono::steady_clock::now() + timeout;
    rollback_ = std::move(rollback_action);
    cv_.notify_all();
}

Result RevertManager::confirm() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!pending_) {
        return Result::failure("No configuration change is currently pending");
    }
    std::string confirmed_id = pending_display_id_;
    pending_ = false;
    rollback_ = nullptr;
    cv_.notify_all();
    lock.unlock();

    events_.push(ConfigurationConfirmed{confirmed_id});
    return Result::success();
}

Result RevertManager::revert() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!pending_) {
        return Result::failure("No configuration change is currently pending");
    }
    cancel_and_revert_locked(lock, "User reverted configuration");
    return Result::success();
}

bool RevertManager::is_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

void RevertManager::cancel_and_revert_locked(std::unique_lock<std::mutex>& lock, const std::string& reason) {
    if (!pending_ || !rollback_) return;
    auto rollback = std::move(rollback_);
    auto display_id = std::move(pending_display_id_);
    pending_ = false;
    rollback_ = nullptr;
    cv_.notify_all();

    lock.unlock();
    rollback();
    events_.push(ConfigurationReverted{display_id, reason});
    lock.lock();
}

void RevertManager::timer_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (!pending_) {
            cv_.wait(lock, [&] { return stopping_ || pending_; });
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline_) {
            // Expired! Roll back
            cancel_and_revert_locked(lock, "Confirmation timeout expired");
            continue;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
        auto step = std::min(remaining, std::chrono::milliseconds(500));
        
        // Report countdown
        std::string cur_id = pending_display_id_;
        lock.unlock();
        events_.push(RevertCountdown{cur_id, remaining});
        lock.lock();

        if (cv_.wait_for(lock, step, [&] { return stopping_ || !pending_; })) {
            // Notified early because stopping or pending cancelled/confirmed
            continue;
        }
    }
}

} // namespace brodisplays
