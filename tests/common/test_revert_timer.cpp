#include "check.h"
#include "brodisplays/event_queue.h"
#include "common/revert_timer.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

const brodisplays::ConfigurationReverted* find_revert(
    const std::vector<brodisplays::DisplayEvent>& events, const std::string& id) {
    for (const auto& ev : events) {
        if (auto* rev = std::get_if<brodisplays::ConfigurationReverted>(&ev)) {
            if (rev->display_id == id) return rev;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    std::printf("[test_revert_timer] Starting RevertManager verification...\n");

    using brodisplays::Result;
    brodisplays::MessageQueue<brodisplays::DisplayEvent> queue;

    // 1. Timeout expiration triggers rollback
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};

        manager.start_temporary(
            "disp-test", std::chrono::milliseconds(200),
            [&] { rolled_back = true; return Result::success(); });

        CHECK(manager.is_pending());

        bool ok = bstest::wait_until([&] { return !manager.is_pending(); }, std::chrono::milliseconds(1000));
        CHECK(ok);
        CHECK(rolled_back.load());

        auto events = queue.drain();
        const auto* rev = find_revert(events, "disp-test");
        CHECK(rev != nullptr);
        if (rev) CHECK(rev->restored);
    }

    // 2. Regression: a slow rollback (a real mode switch takes hundreds of ms)
    //    must keep the configuration pending until the rollback has finished
    //    and the ConfigurationReverted event is queued. Before the fix,
    //    !is_pending() flipped first, so a caller read the still-applied test
    //    mode and found no event.
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};

        manager.start_temporary(
            "disp-slow", std::chrono::milliseconds(100),
            [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                rolled_back = true;
                return Result::success();
            });

        bool ok = bstest::wait_until([&] { return !manager.is_pending(); },
                                     std::chrono::milliseconds(3000), std::chrono::milliseconds(1));
        CHECK(ok);
        CHECK(rolled_back.load());
        CHECK(find_revert(queue.drain(), "disp-slow") != nullptr);

        // confirm() racing an in-flight rollback must not claim success.
        manager.start_temporary(
            "disp-race", std::chrono::milliseconds(50),
            [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                return Result::success();
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(150)); // rollback now in flight
        auto confirm_res = manager.confirm();
        CHECK(!confirm_res.ok);
        CHECK(!manager.is_pending());
        CHECK(find_revert(queue.drain(), "disp-race") != nullptr);
    }

    // 3. A failed rollback is reported, not swallowed
    {
        brodisplays::RevertManager manager(queue);
        manager.start_temporary(
            "disp-fail", std::chrono::milliseconds(5000),
            [] { return Result::failure("driver refused"); });
        auto res = manager.revert();
        CHECK(!res.ok);
        CHECK_EQ(res.error, std::string("driver refused"));
        auto events = queue.drain();
        const auto* rev = find_revert(events, "disp-fail");
        CHECK(rev != nullptr);
        if (rev) {
            CHECK(!rev->restored);
            CHECK_EQ(rev->error, std::string("driver refused"));
        }
        CHECK(!manager.is_pending());
    }

    // 4. Explicit confirm() cancels rollback
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};

        manager.start_temporary(
            "disp-test-2", std::chrono::milliseconds(1000),
            [&] { rolled_back = true; return Result::success(); });

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
        CHECK(find_revert(events, "disp-test-2") == nullptr);
    }

    // 5. Explicit revert() triggers rollback immediately
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};

        manager.start_temporary(
            "disp-test-3", std::chrono::milliseconds(2000),
            [&] { rolled_back = true; return Result::success(); });

        CHECK(manager.is_pending());
        auto rev_res = manager.revert();
        CHECK(rev_res.ok);
        CHECK(rolled_back.load());
        CHECK(!manager.is_pending());
        CHECK(!manager.revert().ok);
    }

    // 6. A second temporary change supersedes (and reverts) the first
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<int> first_rolled{0};
        manager.start_temporary("disp-a", std::chrono::milliseconds(5000),
                                [&] { ++first_rolled; return Result::success(); });
        manager.start_temporary("disp-b", std::chrono::milliseconds(5000),
                                [] { return Result::success(); });
        CHECK_EQ(first_rolled.load(), 1);
        CHECK(manager.is_pending());
        CHECK(find_revert(queue.drain(), "disp-a") != nullptr);
        CHECK(manager.confirm().ok);
    }

    // 8. confirm() runs the persist step; its failure keeps the configuration
    //    pending (the timer then still reverts it) and is reported.
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<int> persisted{0};
        std::atomic<bool> rolled_back{false};
        manager.start_temporary(
            "disp-persist", std::chrono::milliseconds(5000),
            [&] { rolled_back = true; return Result::success(); },
            [&] { ++persisted; return Result::success(); });
        CHECK(manager.confirm().ok);
        CHECK_EQ(persisted.load(), 1);
        CHECK(!manager.is_pending());
        CHECK(!rolled_back.load());
        queue.drain();

        manager.start_temporary(
            "disp-persist-fail", std::chrono::milliseconds(300),
            [&] { rolled_back = true; return Result::success(); },
            [] { return Result::failure("database refused"); });
        auto res = manager.confirm();
        CHECK(!res.ok);
        CHECK(res.error.find("database refused") != std::string::npos);
        CHECK(manager.is_pending());
        bool reverted = bstest::wait_until([&] { return !manager.is_pending(); }, std::chrono::milliseconds(2000));
        CHECK(reverted);
        CHECK(rolled_back.load());
        auto events = queue.drain();
        CHECK(find_revert(events, "disp-persist-fail") != nullptr);
        for (const auto& ev : events) CHECK(!std::holds_alternative<brodisplays::ConfigurationConfirmed>(ev));
    }

    // 9. The timer does not fire while a (slow) persist step runs, even past
    //    the deadline: a confirmation that started in time wins.
    {
        brodisplays::RevertManager manager(queue);
        std::atomic<bool> rolled_back{false};
        manager.start_temporary(
            "disp-slow-persist", std::chrono::milliseconds(100),
            [&] { rolled_back = true; return Result::success(); },
            [] {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                return Result::success();
            });
        CHECK(manager.confirm().ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!rolled_back.load());
        CHECK(!manager.is_pending());
        CHECK(find_revert(queue.drain(), "disp-slow-persist") == nullptr);
    }

    // 7. Destruction rolls back unconfirmed temporary changes
    {
        std::atomic<bool> rolled_back{false};
        {
            brodisplays::RevertManager manager(queue);
            manager.start_temporary(
                "disp-test-4", std::chrono::milliseconds(5000),
                [&] { rolled_back = true; return Result::success(); });
            CHECK(manager.is_pending());
        } // manager destroyed here
        CHECK(rolled_back.load());
    }

    return bstest::finish("test_revert_timer");
}
