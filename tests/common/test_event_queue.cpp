#include "check.h"
#include "brodisplays/event_queue.h"

#include <atomic>
#include <thread>

int main() {
    std::printf("[test_event_queue] Starting MessageQueue verification...\n");

    brodisplays::MessageQueue<brodisplays::DisplayEvent> queue;
    CHECK_EQ(queue.size(), 0u);

    // 1. Drain empty queue
    auto items = queue.drain();
    CHECK(items.empty());

    // 2. Set wake callback
    std::atomic<int> wake_count{0};
    queue.set_wake([&] { ++wake_count; });

    // 3. Push DisplaysChanged event
    brodisplays::DisplaysSnapshot snap;
    brodisplays::DisplayInfo d1;
    d1.id = "disp-1";
    d1.name = "Primary Monitor";
    d1.is_primary = true;
    snap.displays.push_back(d1);

    queue.push(brodisplays::DisplaysChanged{snap});
    CHECK_EQ(queue.size(), 1u);
    CHECK_EQ(wake_count.load(), 1);

    // 4. Push RevertCountdown and ConfigurationReverted events
    queue.push(brodisplays::RevertCountdown{"disp-1", std::chrono::milliseconds(5000)});
    queue.push(brodisplays::ConfigurationReverted{"disp-1", "Timeout", true, {}});
    CHECK_EQ(queue.size(), 3u);
    CHECK_EQ(wake_count.load(), 3);

    // 5. Drain items and verify FIFO order and variants
    auto drained = queue.drain();
    CHECK_EQ(drained.size(), 3u);
    CHECK_EQ(queue.size(), 0u);

    auto* e0 = std::get_if<brodisplays::DisplaysChanged>(&drained[0]);
    REQUIRE(e0 != nullptr);
    CHECK_EQ(e0->snapshot.displays.size(), 1u);
    CHECK_EQ(e0->snapshot.displays[0].id, "disp-1");

    auto* e1 = std::get_if<brodisplays::RevertCountdown>(&drained[1]);
    REQUIRE(e1 != nullptr);
    CHECK_EQ(e1->display_id, "disp-1");
    CHECK_EQ(e1->remaining.count(), 5000);

    auto* e2 = std::get_if<brodisplays::ConfigurationReverted>(&drained[2]);
    REQUIRE(e2 != nullptr);
    CHECK_EQ(e2->display_id, "disp-1");
    CHECK_EQ(e2->reason, "Timeout");

    // 6. Test wait_for timeout
    bool wait_res = queue.wait_for(std::chrono::milliseconds(20));
    CHECK(!wait_res);

    // 7. Test producer on separate thread
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        queue.push(brodisplays::ConfigurationConfirmed{"disp-1"});
    });

    bool wait_success = queue.wait_for(std::chrono::milliseconds(500));
    CHECK(wait_success);
    producer.join();

    auto last_items = queue.drain();
    REQUIRE(last_items.size() == 1u);
    auto* e3 = std::get_if<brodisplays::ConfigurationConfirmed>(&last_items[0]);
    REQUIRE(e3 != nullptr);
    CHECK_EQ(e3->display_id, "disp-1");

    // 8. Test BrightnessEvent in DisplayEvent queue
    brodisplays::BrightnessEvent b_ev;
    b_ev.device_name = "intel_backlight";
    b_ev.connector_name = "eDP-1";
    b_ev.raw_brightness = 15000;
    b_ev.max_brightness = 20000;
    b_ev.min_brightness = 0;
    b_ev.normalized_brightness = 0.75;

    queue.push(b_ev);
    auto b_items = queue.drain();
    REQUIRE(b_items.size() == 1u);
    auto* e4 = std::get_if<brodisplays::BrightnessEvent>(&b_items[0]);
    REQUIRE(e4 != nullptr);
    CHECK_EQ(e4->device_name, "intel_backlight");
    CHECK_EQ(e4->connector_name, "eDP-1");
    CHECK_EQ(e4->raw_brightness, 15000u);
    CHECK(std::abs(e4->normalized_brightness - 0.75) < 1e-6);

    // 9. Dedicated MessageQueue<BrightnessEvent>
    brodisplays::MessageQueue<brodisplays::BrightnessEvent> dedicated_queue;
    dedicated_queue.push(b_ev);
    auto dedicated_items = dedicated_queue.drain();
    REQUIRE(dedicated_items.size() == 1u);
    CHECK_EQ(dedicated_items[0].device_name, "intel_backlight");

    return bstest::finish("test_event_queue");
}
