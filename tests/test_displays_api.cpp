#include "../src/api/api.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "brodisplays/brodisplays.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")"   \
                      << std::endl;                                            \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

int main() {
    namespace ev = bronze::embed;
    using namespace bronze::eval;

    std::cout << "Starting brodisplays JavaScript API tests..." << std::endl;

    // 1. Install bro.displays into Bronze realm
    brodisplays::api::installDisplays();

    // Night light (the gamma ramp), brightness and display configuration act on
    // the live display, so they run only with BRODISPLAYS_TEST_MUTATE=1, like
    // the native mutation tests; otherwise only the getters' shapes are checked.
    {
        const char* v = std::getenv("BRODISPLAYS_TEST_MUTATE");
        const bool mutate = v && std::string(v) == "1";
        evalScript(mutate ? "globalThis.__brodisplaysMutate = true;" : "globalThis.__brodisplaysMutate = false;");
        if (!mutate) {
            std::cout << "Note: setNightLight, setBrightness and applyConfig/testConfig not called; "
                         "set BRODISPLAYS_TEST_MUTATE=1 to run them" << std::endl;
        }
    }

    auto g = ev::globalValue("bro");
    CHECK(g.found);
    CHECK(ev::isObject(g.value));

    ev::Persistent displays(ev::getProperty(g.value, "displays"));
    CHECK(ev::isObject(displays.get()));
    std::cout << "  Mounted bro.displays successfully." << std::endl;

    // Verify all core methods exist
    const char* methods[] = {
        "getSnapshot", "applyConfig", "testConfig", "confirmConfig",
        "revertConfig", "isRevertPending", "setNightLight", "getNightLight",
        "setBrightness", "getBrightness", "on", "off", "addEventListener",
        "removeEventListener", "addListener", "removeListener", "getBacklightDevices"
    };
    for (const char* m : methods) {
        auto fn = ev::getProperty(displays.get(), m);
        CHECK(ev::isFunction(fn));
        std::cout << "  Found bro.displays." << m << std::endl;
    }

    // 2. Test getSnapshot()
    std::cout << "Testing getSnapshot()..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const snap = bro.displays.getSnapshot();\n"
            "  if (typeof snap !== 'object' || snap === null) return 'snap not object';\n"
            "  if (typeof snap.timestamp !== 'number' || snap.timestamp <= 0) return 'invalid timestamp';\n"
            "  if (!Array.isArray(snap.displays)) return 'displays not array';\n"
            "  for (const d of snap.displays) {\n"
            "    if (typeof d.id !== 'string') return 'invalid id';\n"
            "    if (typeof d.name !== 'string') return 'invalid name';\n"
            "    if (typeof d.isPrimary !== 'boolean') return 'invalid isPrimary';\n"
            "    if (typeof d.isConnected !== 'boolean') return 'invalid isConnected';\n"
            "    if (typeof d.geometry !== 'object') return 'invalid geometry';\n"
            "    if (typeof d.geometry.width !== 'number') return 'invalid geom width';\n"
            "    if (typeof d.geometry.height !== 'number') return 'invalid geom height';\n"
            "    if (typeof d.position !== 'object') return 'invalid position';\n"
            "    if (typeof d.scale !== 'object') return 'invalid scale';\n"
            "    if (typeof d.orientation !== 'string') return 'invalid orientation';\n"
            "    if (typeof d.currentMode !== 'object') return 'invalid currentMode';\n"
            "    if (typeof d.currentMode.width !== 'number') return 'invalid mode width';\n"
            "    if (typeof d.currentMode.height !== 'number') return 'invalid mode height';\n"
            "    if (typeof d.currentMode.refreshRate !== 'number') return 'invalid mode refreshRate';\n"
            "    if (!Array.isArray(d.availableModes)) return 'availableModes not array';\n"
            "    if (typeof d.hdr !== 'object') return 'invalid hdr';\n"
            "    if (typeof d.nightLight !== 'object') return 'invalid nightLight';\n"
            "    if (typeof d.edid !== 'object') return 'invalid edid';\n"
            "  }\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        std::string res = ev::toUtf8(r.value);
        CHECK(res == "ok");
        std::cout << "  getSnapshot() passed validation." << std::endl;
    }

    // 3. Test getNightLight() & setNightLight()
    std::cout << "Testing night light controls..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const initial = bro.displays.getNightLight();\n"
            "  if (typeof initial !== 'object' || initial === null) return 'initial not object';\n"
            "  if (typeof initial.supported !== 'boolean') return 'invalid supported';\n"
            "  if (typeof initial.enabled !== 'boolean') return 'invalid enabled';\n"
            "  if (typeof initial.temperature !== 'number') return 'invalid temp';\n"
            "  if (typeof initial.schedule !== 'object') return 'invalid schedule';\n"
            "  if (!globalThis.__brodisplaysMutate) return 'ok';\n"
            "\n"
            "  const setRes = bro.displays.setNightLight({\n"
            "    enabled: true,\n"
            "    temperature: 3200,\n"
            "    schedule: {\n"
            "      startHour: 21,\n"
            "      startMinute: 45,\n"
            "      endHour: 6,\n"
            "      endMinute: 30\n"
            "    }\n"
            "  });\n"
            "  if (typeof setRes !== 'boolean') return 'setRes not boolean';\n"
            "\n"
            "  const updated = bro.displays.getNightLight();\n"
            "  if (updated.temperature !== 3200) return 'temp not updated: ' + updated.temperature;\n"
            "  if (updated.enabled !== true) return 'enabled not updated';\n"
            "  if (updated.schedule.startHour !== 21) return 'schedule startHour mismatch';\n"
            "  if (updated.schedule.startMinute !== 45) return 'schedule startMinute mismatch';\n"
            "  if (updated.schedule.endHour !== 6) return 'schedule endHour mismatch';\n"
            "  if (updated.schedule.endMinute !== 30) return 'schedule endMinute mismatch';\n"
            "\n"
            "  bro.displays.setNightLight(false, 6500);\n"
            "  const reset = bro.displays.getNightLight();\n"
            "  if (reset.temperature !== 6500) return 'temp not reset';\n"
            "  if (reset.enabled !== false) return 'enabled not reset';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        std::string res = ev::toUtf8(r.value);
        CHECK(res == "ok");
        std::cout << "  Night light tests passed." << std::endl;
    }

    // 4. Test brightness controls
    std::cout << "Testing brightness controls..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const b = bro.displays.getBrightness();\n"
            "  if (typeof b !== 'number') return 'getBrightness not number';\n"
            "\n"
            "  if (globalThis.__brodisplaysMutate) {\n"
            "    const setRes = bro.displays.setBrightness(75);\n"
            "    if (typeof setRes !== 'boolean') return 'setBrightness not boolean';\n"
            "  }\n"
            "\n"
            "  const devs = bro.displays.getBacklightDevices();\n"
            "  if (!Array.isArray(devs)) return 'getBacklightDevices not array';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        std::string res = ev::toUtf8(r.value);
        CHECK(res == "ok");
        std::cout << "  Brightness tests passed." << std::endl;
    }

    // 5. Test Event Listener lifecycle
    std::cout << "Testing event listeners..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  let changeCount = 0;\n"
            "  let lastEvent = null;\n"
            "  const listener = function(e) {\n"
            "    changeCount++;\n"
            "    lastEvent = e;\n"
            "  };\n"
            "\n"
            "  const handle = bro.displays.on('change', listener);\n"
            "  if (typeof handle !== 'object' || typeof handle.id !== 'number') return 'invalid handle';\n"
            "\n"
            "  const handle2 = bro.displays.on('countdown', function() {});\n"
            "  const handle3 = bro.displays.on('reverted', function() {});\n"
            "  const handle4 = bro.displays.on('confirmed', function() {});\n"
            "  const handle5 = bro.displays.on('brightness', function() {});\n"
            "\n"
            "  const offRes = bro.displays.off(handle);\n"
            "  if (typeof offRes !== 'boolean' || !offRes) return 'off handle failed';\n"
            "\n"
            "  const offTypeRes = bro.displays.off('countdown');\n"
            "  if (typeof offTypeRes !== 'boolean' || !offTypeRes) return 'off countdown failed';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        std::string res = ev::toUtf8(r.value);
        CHECK(res == "ok");

        brodisplays::api::tickDisplaysAsync();
        std::cout << "  Event listeners and tick passed." << std::endl;
    }

    // 6. Test applyConfig and testConfig Promise APIs
    std::cout << "Testing applyConfig and testConfig..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  if (!globalThis.__brodisplaysMutate) return 'ok';\n"
            "  const snap = bro.displays.getSnapshot();\n"
            "  const primary = snap.displays.find(d => d.isPrimary) || snap.displays[0];\n"
            "  const targetId = primary ? primary.id : 'test-display';\n"
            "  const width = primary ? primary.currentMode.width : 1920;\n"
            "  const height = primary ? primary.currentMode.height : 1080;\n"
            "\n"
            "  const pApply = bro.displays.applyConfig({\n"
            "    displayId: targetId,\n"
            "    width: width,\n"
            "    height: height,\n"
            "    orientation: 'normal'\n"
            "  });\n"
            "  if (!(pApply instanceof Promise)) return 'applyConfig did not return Promise';\n"
            "\n"
            "  const pTest = bro.displays.testConfig({\n"
            "    displayId: targetId,\n"
            "    width: width,\n"
            "    height: height\n"
            "  }, { revertAfterMs: 5000 });\n"
            "  if (!(pTest instanceof Promise)) return 'testConfig did not return Promise';\n"
            "\n"
            "  const pending = bro.displays.isRevertPending();\n"
            "  if (typeof pending !== 'boolean') return 'isRevertPending not boolean';\n"
            "\n"
            "  const revertRes = bro.displays.revertConfig();\n"
            "  if (typeof revertRes !== 'boolean') return 'revertConfig not boolean';\n"
            "\n"
            "  const confirmRes = bro.displays.confirmConfig();\n"
            "  if (typeof confirmRes !== 'boolean') return 'confirmConfig not boolean';\n"
            "\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        std::string res = ev::toUtf8(r.value);
        CHECK(res == "ok");

        brodisplays::api::tickDisplaysAsync();
        if (ev::microtasksPending()) ev::drainMicrotasks();
        std::cout << "  Configuration promise methods passed." << std::endl;
    }

    // 7. GC Stress loop to verify Persistent rooting across garbage collections
    std::cout << "Testing GC safety across multiple allocations..." << std::endl;
    for (int i = 0; i < 50; ++i) {
        auto r = evalScript(
            "(function() {\n"
            "  const snap = bro.displays.getSnapshot();\n"
            "  const nl = bro.displays.getNightLight();\n"
            "  if (globalThis.__brodisplaysMutate)\n"
            "    bro.displays.setNightLight({ enabled: false, temperature: 4000 + (Math.random() * 2000 | 0) });\n"
            "  bro.displays.getBrightness();\n"
            "  return snap.displays.length >= 0 ? 'ok' : 'err';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        brodisplays::api::tickDisplaysAsync();
        if (ev::microtasksPending()) ev::drainMicrotasks();
    }
    std::cout << "  GC safety loop passed." << std::endl;

    // 8. Test BacklightWatcher lifecycle on bro.displays.on('brightness'), setDisplayService, and shutdownDisplaysAsync
    std::cout << "Testing BacklightWatcher lifecycle..." << std::endl;
    {
        struct MockWatcher : public brodisplays::BacklightWatcher {
            brodisplays::MessageQueue<brodisplays::DisplayEvent>* queue = nullptr;
            bool running = true;
            int* stopCount = nullptr;
            MockWatcher(brodisplays::MessageQueue<brodisplays::DisplayEvent>* q, int* sc)
                : queue(q), stopCount(sc) {}
            bool is_running() const noexcept override { return running; }
            void stop() override {
                if (running) {
                    running = false;
                    if (stopCount) (*stopCount)++;
                }
            }
        };

        struct MockBacklightManager : public brodisplays::BacklightManager {
            brodisplays::MessageQueue<brodisplays::DisplayEvent>* activeQueue = nullptr;
            int createCount = 0;
            int stopCount = 0;
            std::vector<brodisplays::BacklightDevice> enumerate_devices() const override { return {}; }
            std::optional<brodisplays::BacklightDevice> find_device_by_name(std::string_view) const override { return std::nullopt; }
            std::optional<brodisplays::BacklightDevice> find_device_for_connector(std::string_view) const override { return std::nullopt; }
            std::optional<brodisplays::BacklightDevice> primary_device() const override { return std::nullopt; }
            brodisplays::Result get_brightness(std::string_view, uint32_t& r, double& n, brodisplays::BrightnessCurve) const override {
                r = 50; n = 0.5; return brodisplays::Result::success();
            }
            brodisplays::Result set_brightness(std::string_view, uint32_t) override { return brodisplays::Result::success(); }
            brodisplays::Result set_normalized_brightness(std::string_view, double, brodisplays::BrightnessCurve) override {
                return brodisplays::Result::success();
            }
            std::unique_ptr<brodisplays::BacklightWatcher> create_watcher(std::function<void(const brodisplays::BrightnessEvent&)>) override { return nullptr; }
            std::unique_ptr<brodisplays::BacklightWatcher> create_watcher(std::string_view, std::function<void(const brodisplays::BrightnessEvent&)>) override { return nullptr; }
            std::unique_ptr<brodisplays::BacklightWatcher> create_watcher(brodisplays::MessageQueue<brodisplays::BrightnessEvent>&) override { return nullptr; }
            std::unique_ptr<brodisplays::BacklightWatcher> create_watcher(std::string_view, brodisplays::MessageQueue<brodisplays::BrightnessEvent>&) override { return nullptr; }
            std::unique_ptr<brodisplays::BacklightWatcher> create_watcher(brodisplays::MessageQueue<brodisplays::DisplayEvent>& q) override {
                createCount++;
                activeQueue = &q;
                return std::make_unique<MockWatcher>(&q, &stopCount);
            }
            std::unique_ptr<brodisplays::BacklightWatcher> create_watcher(std::string_view, brodisplays::MessageQueue<brodisplays::DisplayEvent>& q) override {
                createCount++;
                activeQueue = &q;
                return std::make_unique<MockWatcher>(&q, &stopCount);
            }
        };

        struct MockDisplayService : public brodisplays::DisplayService {
            brodisplays::MessageQueue<brodisplays::DisplayEvent> evs;
            brodisplays::DisplaysSnapshot snapshot() const override { return {}; }
            brodisplays::MessageQueue<brodisplays::DisplayEvent>& events() override { return evs; }
            brodisplays::Result apply_configuration(const brodisplays::DisplayConfigChange&) override { return brodisplays::Result::success(); }
            brodisplays::Result apply_temporary_configuration(const brodisplays::DisplayConfigChange&, std::chrono::milliseconds) override { return brodisplays::Result::success(); }
            brodisplays::Result confirm_configuration() override { return brodisplays::Result::success(); }
            brodisplays::Result revert_configuration() override { return brodisplays::Result::success(); }
            bool is_revert_pending() const override { return false; }
            brodisplays::Result set_night_light(bool, uint32_t) override { return brodisplays::Result::success(); }
        };

        brodisplays::api::shutdownDisplaysAsync();
        auto svc1 = std::make_shared<MockDisplayService>();
        auto mgr = std::make_shared<MockBacklightManager>();
        brodisplays::api::setDisplayService(svc1);
        brodisplays::api::setBacklightManager(mgr);
        CHECK(mgr->createCount == 0);

        // Subscribing to 'brightness' must start the watcher without setBrightness()
        auto rSub = evalScript(
            "globalThis.__brightEvents = [];\n"
            "bro.displays.on('brightness', (e) => { globalThis.__brightEvents.push(e.percent); });\n"
            "true;\n");
        CHECK(!rSub.thrown);
        CHECK(mgr->createCount == 1);
        CHECK(mgr->activeQueue == &svc1->events());

        // Replacing DisplayService stops the old watcher and rebinds to svc2->events()
        auto svc2 = std::make_shared<MockDisplayService>();
        brodisplays::api::setDisplayService(svc2);
        CHECK(mgr->stopCount == 1);
        CHECK(mgr->createCount == 2);
        CHECK(mgr->activeQueue == &svc2->events());

        brodisplays::BrightnessEvent bev{};
        bev.device_name = "intel_backlight";
        bev.normalized_brightness = 0.65;
        mgr->activeQueue->push(bev);
        brodisplays::api::tickDisplaysAsync();

        auto rCheckEv = evalScript("globalThis.__brightEvents.length === 1 && Math.round(globalThis.__brightEvents[0]) === 65");
        CHECK(!rCheckEv.thrown && ev::toBool(rCheckEv.value));

        // shutdownDisplaysAsync stops the watcher
        brodisplays::api::shutdownDisplaysAsync();
        CHECK(mgr->stopCount == 2);
        brodisplays::api::setDisplayService(nullptr);
        brodisplays::api::setBacklightManager(nullptr);
        std::cout << "  BacklightWatcher lifecycle passed." << std::endl;
    }

    // 9. Test shutdown
    std::cout << "Testing shutdownDisplaysAsync()..." << std::endl;
    brodisplays::api::shutdownDisplaysAsync();
    std::cout << "  shutdownDisplaysAsync() completed successfully." << std::endl;

    std::cout << "All brodisplays JavaScript API tests passed!" << std::endl;
    return 0;
}
