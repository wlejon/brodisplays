#pragma once

#include "embed/embed.h"
#include "brodisplays/brodisplays.h"

#include <memory>
#include <string>
#include <vector>

namespace brodisplays::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

// Service accessors
std::shared_ptr<brodisplays::DisplayService> activeDisplayService();
std::shared_ptr<brodisplays::BacklightManager> activeBacklightManager();

// Error helper
Value makeError(const std::string& msg);

// Conversions
Value displayInfoToJs(const brodisplays::DisplayInfo& info);
Value displaySnapshotToJs(const brodisplays::DisplaysSnapshot& snap);
Value displayModeToJs(const brodisplays::DisplayMode& mode);
Value edidInfoToJs(const brodisplays::EdidInfo& edid);

// Installation subroutines
void installDisplaysOnto(Value displaysObj);
void installNightLightOnto(Value displaysObj);
void installBrightnessOnto(Value displaysObj);

// Event handling
void drainDisplayEvents();
void clearDisplayListeners();
void revertPendingTestIfAny();

} // namespace brodisplays::api
