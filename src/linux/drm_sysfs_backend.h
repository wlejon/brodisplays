#pragma once

#include <vector>
#include "brodisplays/types.h"

namespace brodisplays {

class DrmSysfsBackend {
public:
    static bool is_available();
    static std::vector<DisplayInfo> enumerate();
};

} // namespace brodisplays
