#pragma once

#include <memory>

namespace brodisplays {
class DisplayService;
class BacklightManager;
}

namespace brodisplays::api {

/// Mounts `bro.displays` in the current Bronze realm.
void installDisplays();

/// Pumps async display change events, revert timers, and brightness events on the JS thread.
void tickDisplaysAsync();

/// Cleans up active watchers and reverts pending temporary configurations if any.
void shutdownDisplaysAsync();

/// Sets the DisplayService used by the API (if nullptr, uses DisplayService::create()).
void setDisplayService(std::shared_ptr<brodisplays::DisplayService> service);

/// Gets the DisplayService currently used by the API.
std::shared_ptr<brodisplays::DisplayService> getDisplayService();

/// Sets the BacklightManager used by the API (if nullptr, uses BacklightManager::create()).
void setBacklightManager(std::shared_ptr<brodisplays::BacklightManager> manager);

/// Gets the BacklightManager currently used by the API.
std::shared_ptr<brodisplays::BacklightManager> getBacklightManager();

} // namespace brodisplays::api

using brodisplays::api::installDisplays;
using brodisplays::api::tickDisplaysAsync;
using brodisplays::api::shutdownDisplaysAsync;
using brodisplays::api::setDisplayService;
using brodisplays::api::getDisplayService;
using brodisplays::api::setBacklightManager;
using brodisplays::api::getBacklightManager;
