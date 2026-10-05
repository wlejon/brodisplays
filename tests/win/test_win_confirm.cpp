#include "common/confirm_scenario.h"

#include <vector>

#include <windows.h>

// The persisted store on Windows is the CCD display database:
// QueryDisplayConfig(QDC_DATABASE_CURRENT) answers what Windows would apply
// for the connected monitor set at the next logon or reconnect.

namespace {

std::string utf8(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::optional<brodisplays::DisplayMode> database_mode(const brodisplays::DisplayInfo& d) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &np, &nm) != ERROR_SUCCESS) return std::nullopt;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    DISPLAYCONFIG_TOPOLOGY_ID topology{};
    if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &np, paths.data(), &nm, modes.data(), &topology) != ERROR_SUCCESS)
        return std::nullopt;
    for (UINT32 i = 0; i < np; ++i) {
        const auto& p = paths[i];
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS || utf8(tn.monitorDevicePath) != d.id) continue;
        brodisplays::DisplayMode m;
        if (p.sourceInfo.modeInfoIdx < nm && modes[p.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            m.width = modes[p.sourceInfo.modeInfoIdx].sourceMode.width;
            m.height = modes[p.sourceInfo.modeInfoIdx].sourceMode.height;
        }
        if (p.targetInfo.modeInfoIdx < nm && modes[p.targetInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            const auto& v = modes[p.targetInfo.modeInfoIdx].targetMode.targetVideoSignalInfo.vSyncFreq;
            if (v.Denominator) m.refresh_rate = double(v.Numerator) / double(v.Denominator);
        }
        if (m.width == 0) return std::nullopt;
        return m;
    }
    return std::nullopt;
}

} // namespace

int main() {
    std::printf("[test_win_confirm] Windows confirm-persistence verification\n");
    return bdtest::run_confirm_scenario("test_win_confirm", database_mode);
}
