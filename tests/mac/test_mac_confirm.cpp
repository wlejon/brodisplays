#include "common/confirm_scenario.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ColorSync/ColorSync.h>

#include <set>
#include <string>
#include <vector>

// The persisted store on macOS is WindowServer's display preferences
// (com.apple.windowserver.displays: the per-user ByHost domain, and the
// any-user domain in /Library/Preferences). Each "DisplaySets" config is one
// set of displays by UUID; its CurrentInfo is what WindowServer restores for
// that display in that set: Wide x High points at Scale, Hz.

namespace {

std::string cf_string(CFStringRef s) {
    if (!s) return "";
    char buf[256];
    return CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8) ? std::string(buf) : std::string();
}

std::string display_uuid(CGDirectDisplayID d) {
    CFUUIDRef u = CGDisplayCreateUUIDFromDisplayID(d);
    if (!u) return "";
    CFStringRef s = CFUUIDCreateString(nullptr, u);
    std::string out = cf_string(s);
    if (s) CFRelease(s);
    CFRelease(u);
    return out;
}

double number(CFDictionaryRef d, const char* key) {
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    auto n = (CFNumberRef)CFDictionaryGetValue(d, k);
    CFRelease(k);
    double v = 0;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberDoubleType, &v);
    return v;
}

std::optional<brodisplays::DisplayMode> mode_in(CFPropertyListRef sets, const std::set<std::string>& online,
                                                const std::string& uuid) {
    if (!sets || CFGetTypeID(sets) != CFDictionaryGetTypeID()) return std::nullopt;
    auto configs = (CFArrayRef)CFDictionaryGetValue((CFDictionaryRef)sets, CFSTR("Configs"));
    if (!configs || CFGetTypeID(configs) != CFArrayGetTypeID()) return std::nullopt;
    for (CFIndex i = 0; i < CFArrayGetCount(configs); ++i) {
        auto config = (CFDictionaryRef)CFArrayGetValueAtIndex(configs, i);
        auto displays = (CFArrayRef)CFDictionaryGetValue(config, CFSTR("DisplayConfig"));
        if (!displays) continue;
        std::set<std::string> uuids;
        CFDictionaryRef ours = nullptr;
        for (CFIndex j = 0; j < CFArrayGetCount(displays); ++j) {
            auto entry = (CFDictionaryRef)CFArrayGetValueAtIndex(displays, j);
            std::string u = cf_string((CFStringRef)CFDictionaryGetValue(entry, CFSTR("UUID")));
            uuids.insert(u);
            if (u == uuid) ours = entry;
        }
        if (uuids != online || !ours) continue;
        auto info = (CFDictionaryRef)CFDictionaryGetValue(ours, CFSTR("CurrentInfo"));
        if (!info) continue;
        double scale = number(info, "Scale");
        if (scale <= 0) scale = 1;
        brodisplays::DisplayMode m;
        m.width = static_cast<uint32_t>(number(info, "Wide") * scale);
        m.height = static_cast<uint32_t>(number(info, "High") * scale);
        m.refresh_rate = number(info, "Hz");
        return m;
    }
    return std::nullopt;
}

std::optional<brodisplays::DisplayMode> persisted_mode(const brodisplays::DisplayInfo& d) {
    CGDirectDisplayID id = static_cast<CGDirectDisplayID>(std::stoul(d.id));
    std::set<std::string> online;
    uint32_t count = 0;
    CGDirectDisplayID ids[16];
    CGGetOnlineDisplayList(16, ids, &count);
    for (uint32_t i = 0; i < count; ++i) online.insert(display_uuid(ids[i]));
    const std::string uuid = display_uuid(id);

    CFStringRef domain = CFSTR("com.apple.windowserver.displays");
    struct Where {
        CFStringRef user;
        CFStringRef host;
    };
    for (Where w : {Where{kCFPreferencesCurrentUser, kCFPreferencesCurrentHost},
                    Where{kCFPreferencesAnyUser, kCFPreferencesAnyHost}}) {
        CFPreferencesSynchronize(domain, w.user, w.host);
        CFPropertyListRef sets = CFPreferencesCopyValue(CFSTR("DisplaySets"), domain, w.user, w.host);
        auto m = mode_in(sets, online, uuid);
        if (sets) CFRelease(sets);
        if (m) return m;
    }
    return std::nullopt;
}

} // namespace

int main() {
    std::printf("[test_mac_confirm] macOS confirm-persistence verification\n");
    return bdtest::run_confirm_scenario("test_mac_confirm", persisted_mode);
}
