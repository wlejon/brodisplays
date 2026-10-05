#include "drm_sysfs_backend.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "brodisplays/edid.h"

namespace brodisplays {

namespace fs = std::filesystem;

namespace {

std::string read_trimmed_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return "";
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

std::vector<std::string> read_all_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream f(p);
    if (!f.is_open()) return lines;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::vector<uint8_t> read_binary_file(const fs::path& p) {
    std::vector<uint8_t> data;
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return data;
    f.seekg(0, std::ios::end);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size > 0 && size <= 1024) {
        data.resize(static_cast<size_t>(size));
        f.read(reinterpret_cast<char*>(data.data()), size);
    }
    return data;
}

DisplayMode parse_mode_string(const std::string& mode_str, double default_rr = 60.0) {
    DisplayMode m;
    auto x_pos = mode_str.find('x');
    if (x_pos != std::string::npos) {
        try {
            m.width = static_cast<uint32_t>(std::stoul(mode_str.substr(0, x_pos)));
            m.height = static_cast<uint32_t>(std::stoul(mode_str.substr(x_pos + 1)));
            m.refresh_rate = default_rr;
        } catch (...) {}
    }
    return m;
}

} // namespace

bool DrmSysfsBackend::is_available() {
    std::error_code ec;
    return fs::exists("/sys/class/drm", ec);
}

std::vector<DisplayInfo> DrmSysfsBackend::enumerate() {
    std::vector<DisplayInfo> results;
    std::error_code ec;
    if (!fs::exists("/sys/class/drm", ec)) return results;

    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
        if (!entry.is_directory(ec)) continue;

        fs::path status_path = entry.path() / "status";
        if (!fs::exists(status_path, ec)) continue;

        std::string status = read_trimmed_file(status_path);
        if (status != "connected") continue;

        std::string dir_name = entry.path().filename().string(); // e.g. "card0-HDMI-A-1"
        std::string connector_name = dir_name;
        auto dash_pos = dir_name.find('-');
        if (dash_pos != std::string::npos) {
            connector_name = dir_name.substr(dash_pos + 1); // "HDMI-A-1"
        }

        DisplayInfo info;
        info.id = connector_name;
        info.device_name = connector_name;
        info.name = connector_name;
        info.is_connected = true;
        info.is_primary = results.empty(); // First found is default primary
        info.adapter_name = "DRM Kernel Device";

        if (connector_name.find("eDP") != std::string::npos ||
            connector_name.find("LVDS") != std::string::npos ||
            connector_name.find("DSI") != std::string::npos) {
            info.is_internal = true;
        }

        // Modes from sysfs
        fs::path modes_path = entry.path() / "modes";
        auto mode_lines = read_all_lines(modes_path);
        for (const auto& line : mode_lines) {
            DisplayMode m = parse_mode_string(line);
            if (m.width > 0 && m.height > 0) {
                bool exists = false;
                for (const auto& ex : info.available_modes) {
                    if (ex.width == m.width && ex.height == m.height) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    info.available_modes.push_back(m);
                }
            }
        }

        // EDID from sysfs
        fs::path edid_path = entry.path() / "edid";
        auto edid_bytes = read_binary_file(edid_path);
        if (!edid_bytes.empty()) {
            if (parse_edid(edid_bytes.data(), edid_bytes.size(), info.edid)) {
                if (!info.edid.monitor_name.empty()) {
                    info.name = info.edid.monitor_name;
                    info.model = info.edid.monitor_name;
                }
                if (!info.edid.manufacturer_id.empty()) {
                    info.manufacturer = info.edid.manufacturer_id;
                }
                if (info.edid.width_cm > 0 && info.edid.height_cm > 0) {
                    info.physical_width_mm = info.edid.width_cm * 10;
                    info.physical_height_mm = info.edid.height_cm * 10;
                }

                // If EDID modes are available, merge or update refresh rates
                auto edid_modes = extract_edid_modes(edid_bytes.data(), edid_bytes.size());
                for (const auto& em : edid_modes) {
                    for (auto& am : info.available_modes) {
                        if (am.width == em.width && am.height == em.height) {
                            am.refresh_rate = em.refresh_rate;
                        }
                    }
                }
            }
        }

        if (!info.available_modes.empty()) {
            info.current_mode = info.available_modes.front();
            info.geometry.width = info.current_mode.width;
            info.geometry.height = info.current_mode.height;
        }

        info.scale.factor = 1.0;
        info.scale.dpi = 96;

        results.push_back(info);
    }

    return results;
}

} // namespace brodisplays
