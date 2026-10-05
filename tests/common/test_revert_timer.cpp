#include "check.h"
#include "brodisplays/event_queue.h"
#include "common/revert_timer.h"

#include <atomic>
#include <chrono>
#include <thread>

int main() {
    std::printf("[test_revert_timer] Starting RevertManager verification...\n");

    brodisplays::MessageQueue<brodisplays::DisplayEvent> queue;

    // 1. Timeout expiration triggers rollback
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};

        manager.start_temporary(
            "disp-test", std::chrono::milliseconds(200),
            [&] { rolled_back = true; });

        CHECK(manager.is_pending());

        // Wait for timeout to expire
        bool ok = bstest::wait_until([&] { return rolled_back.load(); }, std::chrono::milliseconds(1000));
        CHECK(ok);
        CHECK(!manager.is_pending());

        // Drain queue, verify events
        auto events = queue.drain();
        bool found_revert = false;
        for (const auto& ev : events) {
            if (auto* rev = std::get_if<brodisplays::ConfigurationReverted>(&ev)) {
                if (rev->display_id == "disp-test") {
                    found_revert = true;
                }
            }
        }
        CHECK(found_revert);
    }

    // 2. Explicit confirm() cancels rollback
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};

        manager.start_temporary(
            "disp-test-2", std::chrono::milliseconds(1000),
            [&] { rolled_back = true; });

        CHECK(manager.is_pending());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto confirm_res = manager.confirm();
        CHECK(confirm_res.ok);
        CHECK(!manager.is_pending());

        // Wait past original deadline
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK(!rolled_back.load());

        auto events = queue.drain();
        bool found_confirm = false;
        for (const auto& ev : events) {
            if (auto* conf = std::get_if<brodisplays::ConfigurationConfirmed>(&ev)) {
                if (conf->display_id == "disp-test-2") {
                    found_confirm = true;
                }
            }
        }
        CHECK(found_confirm);
    }

    // 3. Explicit revert() triggers rollback immediately
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};

        manager.start_temporary(
            "disp-test-3", std::chrono::milliseconds(2000),
            [&] { rolled_back = true; });

        CHECK(manager.is_pending());
        auto rev_res = manager.revert();
        CHECK(rev_res.ok);
        CHECK(rolled_back.load());
        CHECK(!manager.is_pending());
    }

    // 4. Destruction rolls back unconfirmed temporary changes
    {
        std::atomic<bool> rolled_back{false};
        {
            brodisplays::RevertManager manager(queue);
            manager.start_temporary(
                "disp-test-4", std::chrono::milliseconds(5000),
                [&] { rolled_back = true; });
            CHECK(manager.is_pending());
        } // manager destroyed here
        CHECK(rolled_back.load());
    }

    return bstest::finish("test_revert_timer");
}
