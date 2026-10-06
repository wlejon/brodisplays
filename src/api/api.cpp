#include "api.h"
#include "host_displays_internal.h"
#include "object_builder.h"
#include "brodisplays/brodisplays.h"

#include <mutex>

namespace brodisplays::api {

namespace {

std::mutex g_services_mu;

std::shared_ptr<brodisplays::DisplayService> g_custom_display_service;
std::shared_ptr<brodisplays::DisplayService> g_default_display_service;

std::shared_ptr<brodisplays::BacklightManager> g_custom_backlight_manager;
std::shared_ptr<brodisplays::BacklightManager> g_default_backlight_manager;

} // namespace

std::shared_ptr<brodisplays::DisplayService> activeDisplayService() {
    std::lock_guard lock(g_services_mu);
    if (g_custom_display_service) return g_custom_display_service;
    if (!g_default_display_service) {
        std::string err;
        auto service = brodisplays::DisplayService::create({}, &err);
        if (service) {
            g_default_display_service = std::shared_ptr<brodisplays::DisplayService>(std::move(service));
        }
    }
    return g_default_display_service;
}

void setDisplayService(std::shared_ptr<brodisplays::DisplayService> service) {
    std::lock_guard lock(g_services_mu);
    g_custom_display_service = std::move(service);
}

std::shared_ptr<brodisplays::DisplayService> getDisplayService() {
    return activeDisplayService();
}

std::shared_ptr<brodisplays::BacklightManager> activeBacklightManager() {
    std::lock_guard lock(g_services_mu);
    if (g_custom_backlight_manager) return g_custom_backlight_manager;
    if (!g_default_backlight_manager) {
        auto mgr = brodisplays::BacklightManager::create();
        if (mgr) {
            g_default_backlight_manager = std::shared_ptr<brodisplays::BacklightManager>(std::move(mgr));
        }
    }
    return g_default_backlight_manager;
}

void setBacklightManager(std::shared_ptr<brodisplays::BacklightManager> manager) {
    std::lock_guard lock(g_services_mu);
    g_custom_backlight_manager = std::move(manager);
}

std::shared_ptr<brodisplays::BacklightManager> getBacklightManager() {
    return activeBacklightManager();
}

Value makeError(const std::string& msg) {
    ev::Persistent text(ev::fromUtf8(msg));
    auto ctor = ev::globalValue("Error");
    if (ctor.found && ev::isFunction(ctor.value)) {
        ev::Persistent c(ctor.value);
        const Value arg = text.get();
        auto r = ev::construct(c.get(), std::span<const Value>(&arg, 1));
        if (!r.thrown) return r.value;
    }
    return text.get();
}


Value ensureBroDisplays() {
    ev::Persistent globalThisVal;
    auto gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        globalThisVal.set(gt.value);
    }

    ev::Persistent broP;
    auto bro = ev::globalValue("bro");
    if (bro.found && ev::isObject(bro.value)) broP.set(bro.value);
    if (!ev::isObject(broP.get()) && ev::isObject(globalThisVal.get())) {
        Value candidate = ev::getProperty(globalThisVal.get(), "bro");
        if (ev::isObject(candidate)) broP.set(candidate);
    }
    if (!ev::isObject(broP.get())) {
        broP.set(ev::createObject());
        ev::registerGlobal("bro", broP.get());
        if (ev::isObject(globalThisVal.get())) {
            globalThisVal.set(ev::setProperty(globalThisVal.get(), "bro", broP.get()));
        }
    }

    ev::Persistent displaysP(ev::getProperty(broP.get(), "displays"));
    if (!ev::isObject(displaysP.get())) {
        displaysP.set(ev::createObject());
        broP.set(ev::setProperty(broP.get(), "displays", displaysP.get()));
    }
    return displaysP.get();
}

void installDisplays() {
    ev::Persistent displaysObj(ensureBroDisplays());
    installDisplaysOnto(displaysObj.get());
    installNightLightOnto(displaysObj.get());
    installBrightnessOnto(displaysObj.get());
}

void tickDisplaysAsync() {
    drainDisplayEvents();
}

void shutdownDisplaysAsync() {
    clearDisplayListeners();
    revertPendingTestIfAny();
}

} // namespace brodisplays::api
