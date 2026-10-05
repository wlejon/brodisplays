#pragma once

// The alternate modes the display-changing tests switch to, and the loop that
// tries them. A display may list modes it will not really take (a virtual
// display that ignores refresh rates, a driver that refuses a resolution); the
// service reports that as a failed apply, and the test moves on to the next
// candidate. Only when every candidate is refused is the test skipped.

#include "check.h"
#include "brodisplays/display_service.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace bdtest {

inline bool same_size(const brodisplays::DisplayMode& a, const brodisplays::DisplayMode& b) {
    return a.width == b.width && a.height == b.height;
}

inline bool same_rate(const brodisplays::DisplayMode& a, const brodisplays::DisplayMode& b) {
    return std::abs(a.refresh_rate - b.refresh_rate) < 1.0;
}

inline void add_unique(std::vector<brodisplays::DisplayMode>& out, const brodisplays::DisplayMode& m) {
    for (const auto& o : out) {
        if (same_size(o, m) && same_rate(o, m)) return;
    }
    out.push_back(m);
}

// Other resolutions (at least 1024x768), those at the current refresh rate
// first: a resolution change that also has to change the rate fails on
// displays that cannot change it.
inline std::vector<brodisplays::DisplayMode> resolution_alternates(const brodisplays::DisplayInfo& d) {
    const auto& cur = d.current_mode;
    std::vector<brodisplays::DisplayMode> out;
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& m : d.available_modes) {
            if (same_size(m, cur) || m.width < 1024 || m.height < 768) continue;
            if (pass == 0 && !same_rate(m, cur)) continue;
            add_unique(out, m);
        }
    }
    return out;
}

// Every candidate, in the order the tests try them: another refresh rate at
// the current resolution (the desktop geometry stays), then the
// resolution_alternates().
inline std::vector<brodisplays::DisplayMode> alternate_modes(const brodisplays::DisplayInfo& d) {
    const auto& cur = d.current_mode;
    std::vector<brodisplays::DisplayMode> out;
    for (const auto& m : d.available_modes) {
        if (same_size(m, cur) && std::abs(m.refresh_rate - cur.refresh_rate) > 5.0) add_unique(out, m);
    }
    for (const auto& m : resolution_alternates(d)) add_unique(out, m);
    return out;
}

inline brodisplays::DisplayConfigChange change_to(const std::string& id, const brodisplays::DisplayMode& m) {
    brodisplays::DisplayConfigChange c;
    c.display_id = id;
    c.width = m.width;
    c.height = m.height;
    c.refresh_rate = m.refresh_rate;
    return c;
}

// Applies the first candidate the platform takes as a temporary
// configuration. Returns its index, or -1 with every refusal in `refusals`.
inline int apply_first_accepted(brodisplays::DisplayService& service, const std::string& id,
                                const std::vector<brodisplays::DisplayMode>& candidates,
                                std::chrono::milliseconds timeout, std::string* refusals) {
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& m = candidates[i];
        std::printf("Applying temporary mode %ux%u @ %.2f Hz (%lld ms revert timeout)...\n", m.width, m.height,
                    m.refresh_rate, static_cast<long long>(timeout.count()));
        auto res = service.apply_temporary_configuration(change_to(id, m), timeout);
        if (res.ok) return static_cast<int>(i);
        std::printf("  refused: %s\n", res.error.c_str());
        if (!refusals->empty()) *refusals += "; ";
        *refusals += res.error;
    }
    return -1;
}

} // namespace bdtest
