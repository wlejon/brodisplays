#include "brodisplays/types.h"

#include <iomanip>
#include <sstream>

namespace brodisplays {

std::string to_string(DisplayOrientation orientation) {
    switch (orientation) {
        case DisplayOrientation::Normal: return "Normal";
        case DisplayOrientation::Rotate90: return "Rotate90";
        case DisplayOrientation::Rotate180: return "Rotate180";
        case DisplayOrientation::Rotate270: return "Rotate270";
    }
    return "Unknown";
}

} // namespace brodisplays
