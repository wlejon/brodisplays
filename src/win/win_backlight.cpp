// Windows backlight brightness backend:
// 1. WMI (root\WMI: WmiMonitorBrightness / WmiMonitorBrightnessMethods::WmiSetBrightness)
//    for integrated laptop/tablet panels.
// 2. DDC/CI (Dxva2.lib: GetPhysicalMonitorsFromHMONITOR / GetMonitorBrightness /
//    SetMonitorBrightness) for external monitors that support VCP brightness.
#include "brodisplays/backlight.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wbemidl.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace brodisplays {

namespace {

std::string wide_to_utf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(std::string_view str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), len);
    return out;
}

class ComScope {
public:
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            uninit_ = true;
        }
    }
    ~ComScope() {
        if (uninit_) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool uninit_ = false;
};

class Bstr {
public:
    explicit Bstr(const wchar_t* s) : b_(SysAllocString(s)) {}
    ~Bstr() {
        if (b_) SysFreeString(b_);
    }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR get() const { return b_; }

private:
    BSTR b_ = nullptr;
};

bool connect_wmi_root(IWbemServices** out_svc) {
    if (!out_svc) return false;
    *out_svc = nullptr;

    IWbemLocator* locator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, reinterpret_cast<void**>(&locator));
    if (FAILED(hr) || !locator) return false;

    IWbemServices* svc = nullptr;
    Bstr ns(L"ROOT\\WMI");
    hr = locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    locator->Release();
    if (FAILED(hr) || !svc) return false;

    hr = CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        svc->Release();
        return false;
    }

    *out_svc = svc;
    return true;
}

std::string sanitize_instance_name(std::string_view instance) {
    std::string out = "wmi:";
    for (char c : instance) {
        if (c == '\\' || c == '/') out.push_back('_');
        else out.push_back(c);
    }
    return out;
}

std::string primary_gdi_connector() {
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
            (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
            return wide_to_utf8(dd.DeviceName);
        }
    }
    return "\\\\.\\DISPLAY1";
}

void enumerate_wmi_backlights(std::vector<BacklightDevice>& out) {
    ComScope com;
    IWbemServices* svc = nullptr;
    if (!connect_wmi_root(&svc)) return;

    IEnumWbemClassObject* enumerator = nullptr;
    Bstr lang(L"WQL");
    Bstr query(L"SELECT InstanceName, CurrentBrightness FROM WmiMonitorBrightness WHERE Active = TRUE");
    HRESULT hr = svc->ExecQuery(lang.get(), query.get(),
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    if (SUCCEEDED(hr) && enumerator) {
        const std::string primary_conn = primary_gdi_connector();
        while (true) {
            IWbemClassObject* obj = nullptr;
            ULONG ret = 0;
            hr = enumerator->Next(2000, 1, &obj, &ret);
            if (FAILED(hr) || ret == 0 || !obj) break;

            VARIANT v_name{};
            VARIANT v_cur{};
            VariantInit(&v_name);
            VariantInit(&v_cur);

            std::string inst;
            uint32_t cur = 0;
            bool ok = false;

            if (SUCCEEDED(obj->Get(L"InstanceName", 0, &v_name, nullptr, nullptr)) &&
                v_name.vt == VT_BSTR && v_name.bstrVal) {
                inst = wide_to_utf8(v_name.bstrVal);
            }
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v_cur, nullptr, nullptr))) {
                if (v_cur.vt == VT_UI1) {
                    cur = v_cur.bVal;
                    ok = true;
                } else if (v_cur.vt == VT_I4 || v_cur.vt == VT_UI4) {
                    cur = static_cast<uint32_t>(v_cur.ulVal);
                    ok = true;
                }
            }

            VariantClear(&v_name);
            VariantClear(&v_cur);
            obj->Release();

            if (ok && !inst.empty()) {
                BacklightDevice dev;
                dev.name = sanitize_instance_name(inst);
                dev.sysfs_path = inst;
                dev.type = BacklightType::Firmware;
                dev.min_brightness = 0;
                dev.max_brightness = 100;
                dev.current_brightness = std::clamp<uint32_t>(cur, 0, 100);
                dev.normalized_brightness = dev.to_normalized(dev.current_brightness);
                dev.associated_connector = primary_conn;
                out.push_back(std::move(dev));
            }
        }
        enumerator->Release();
    }
    svc->Release();
}

Result set_wmi_backlight(const BacklightDevice& dev, uint32_t raw_brightness) {
    ComScope com;
    IWbemServices* svc = nullptr;
    if (!connect_wmi_root(&svc)) {
        return Result::failure("Failed to connect to ROOT\\WMI");
    }

    const uint32_t pct = std::clamp<uint32_t>(raw_brightness, 0, 100);

    IWbemClassObject* cls = nullptr;
    Bstr cls_name(L"WmiMonitorBrightnessMethods");
    HRESULT hr = svc->GetObject(cls_name.get(), 0, nullptr, &cls, nullptr);
    if (FAILED(hr) || !cls) {
        svc->Release();
        return Result::failure("WmiMonitorBrightnessMethods class unavailable");
    }

    IWbemClassObject* in_sig = nullptr;
    hr = cls->GetMethod(L"WmiSetBrightness", 0, &in_sig, nullptr);
    cls->Release();
    if (FAILED(hr) || !in_sig) {
        svc->Release();
        return Result::failure("WmiSetBrightness method signature unavailable");
    }

    IEnumWbemClassObject* enumerator = nullptr;
    Bstr lang(L"WQL");
    Bstr query(L"SELECT __PATH, InstanceName FROM WmiMonitorBrightnessMethods WHERE Active = TRUE");
    hr = svc->ExecQuery(lang.get(), query.get(),
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &enumerator);
    bool applied = false;
    if (SUCCEEDED(hr) && enumerator) {
        while (true) {
            IWbemClassObject* obj = nullptr;
            ULONG ret = 0;
            hr = enumerator->Next(2000, 1, &obj, &ret);
            if (FAILED(hr) || ret == 0 || !obj) break;

            VARIANT v_inst{};
            VARIANT v_path{};
            VariantInit(&v_inst);
            VariantInit(&v_path);

            std::string inst;
            if (SUCCEEDED(obj->Get(L"InstanceName", 0, &v_inst, nullptr, nullptr)) &&
                v_inst.vt == VT_BSTR && v_inst.bstrVal) {
                inst = wide_to_utf8(v_inst.bstrVal);
            }

            if ((dev.sysfs_path.empty() || inst == dev.sysfs_path) &&
                SUCCEEDED(obj->Get(L"__PATH", 0, &v_path, nullptr, nullptr)) &&
                v_path.vt == VT_BSTR && v_path.bstrVal) {
                IWbemClassObject* in_params = nullptr;
                if (SUCCEEDED(in_sig->SpawnInstance(0, &in_params)) && in_params) {
                    VARIANT v_timeout{};
                    v_timeout.vt = VT_I4;
                    v_timeout.lVal = 0;
                    in_params->Put(L"Timeout", 0, &v_timeout, CIM_UINT32);

                    VARIANT v_br{};
                    v_br.vt = VT_UI1;
                    v_br.bVal = static_cast<BYTE>(pct);
                    in_params->Put(L"Brightness", 0, &v_br, CIM_UINT8);

                    Bstr method_name(L"WmiSetBrightness");
                    HRESULT exec_hr = svc->ExecMethod(v_path.bstrVal, method_name.get(), 0,
                                                      nullptr, in_params, nullptr, nullptr);
                    if (SUCCEEDED(exec_hr)) {
                        applied = true;
                    }
                    in_params->Release();
                }
            }

            VariantClear(&v_inst);
            VariantClear(&v_path);
            obj->Release();
            if (applied) break;
        }
        enumerator->Release();
    }

    in_sig->Release();
    svc->Release();
    return applied ? Result::success()
                   : Result::failure("WmiSetBrightness failed for " + dev.name);
}

void enumerate_ddcci_backlights(std::vector<BacklightDevice>& out) {
    struct Ctx {
        std::vector<BacklightDevice>* out;
        int index = 0;
    } ctx{&out, 0};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            std::string connector;
            if (GetMonitorInfoW(hMon, &mi)) {
                connector = wide_to_utf8(mi.szDevice);
            }

            DWORD count = 0;
            if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) {
                return TRUE;
            }
            std::vector<PHYSICAL_MONITOR> phys(count);
            if (!GetPhysicalMonitorsFromHMONITOR(hMon, count, phys.data())) {
                return TRUE;
            }

            for (DWORD i = 0; i < count; ++i) {
                DWORD min_b = 0, cur_b = 0, max_b = 0;
                if (GetMonitorBrightness(phys[i].hPhysicalMonitor, &min_b, &cur_b, &max_b) &&
                    max_b > min_b) {
                    BacklightDevice dev;
                    dev.name = "ddcci_" + std::to_string(c->index++);
                    dev.sysfs_path = wide_to_utf8(phys[i].szPhysicalMonitorDescription);
                    dev.type = BacklightType::Raw;
                    dev.min_brightness = min_b;
                    dev.max_brightness = max_b;
                    dev.current_brightness = std::clamp(cur_b, min_b, max_b);
                    dev.normalized_brightness = dev.to_normalized(dev.current_brightness);
                    dev.associated_connector = connector;
                    c->out->push_back(std::move(dev));
                }
            }

            DestroyPhysicalMonitors(count, phys.data());
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}

Result set_ddcci_backlight(const BacklightDevice& target_dev, uint32_t raw_brightness) {
    struct Ctx {
        const BacklightDevice* target;
        uint32_t raw;
        int index = 0;
        bool applied = false;
    } ctx{&target_dev, raw_brightness, 0, false};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            std::string connector;
            if (GetMonitorInfoW(hMon, &mi)) {
                connector = wide_to_utf8(mi.szDevice);
            }

            DWORD count = 0;
            if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) {
                return TRUE;
            }
            std::vector<PHYSICAL_MONITOR> phys(count);
            if (!GetPhysicalMonitorsFromHMONITOR(hMon, count, phys.data())) {
                return TRUE;
            }

            for (DWORD i = 0; i < count; ++i) {
                DWORD min_b = 0, cur_b = 0, max_b = 0;
                if (GetMonitorBrightness(phys[i].hPhysicalMonitor, &min_b, &cur_b, &max_b) &&
                    max_b > min_b) {
                    std::string dev_name = "ddcci_" + std::to_string(c->index++);
                    if (dev_name == c->target->name ||
                        (!connector.empty() && connector == c->target->associated_connector)) {
                        DWORD clamped = std::clamp<DWORD>(c->raw, min_b, max_b);
                        if (SetMonitorBrightness(phys[i].hPhysicalMonitor, clamped)) {
                            c->applied = true;
                        }
                    }
                }
            }

            DestroyPhysicalMonitors(count, phys.data());
            return c->applied ? FALSE : TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    return ctx.applied ? Result::success()
                       : Result::failure("SetMonitorBrightness failed for " + target_dev.name);
}

std::vector<BacklightDevice> enumerate_win_backlights() {
    std::vector<BacklightDevice> out;
    enumerate_wmi_backlights(out);
    enumerate_ddcci_backlights(out);
    return out;
}

class WinBacklightWatcher final : public BacklightWatcher {
public:
    WinBacklightWatcher(std::string target, std::function<void(const BrightnessEvent&)> on_event)
        : target_(std::move(target)), on_event_(std::move(on_event)) {
        running_.store(true);
        thread_ = std::thread(&WinBacklightWatcher::run_loop, this);
    }

    ~WinBacklightWatcher() override {
        stop();
    }

    bool is_running() const noexcept override {
        return running_.load();
    }

    void stop() override {
        if (running_.exchange(false)) {
            cv_.notify_all();
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

private:
    void run_loop() {
        std::unordered_map<std::string, uint32_t> last_raw;
        for (const auto& d : enumerate_win_backlights()) {
            if (target_.empty() || d.name == target_ || d.associated_connector == target_) {
                last_raw[d.name] = d.current_brightness;
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        while (running_.load()) {
            cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return !running_.load(); });
            if (!running_.load()) break;

            lock.unlock();
            auto current = enumerate_win_backlights();
            for (const auto& d : current) {
                if (!target_.empty() && d.name != target_ && d.associated_connector != target_) {
                    continue;
                }
                auto it = last_raw.find(d.name);
                if (it == last_raw.end()) {
                    last_raw[d.name] = d.current_brightness;
                } else if (it->second != d.current_brightness) {
                    it->second = d.current_brightness;
                    if (on_event_) {
                        BrightnessEvent ev;
                        ev.device_name = d.name;
                        ev.connector_name = d.associated_connector;
                        ev.raw_brightness = d.current_brightness;
                        ev.max_brightness = d.max_brightness;
                        ev.min_brightness = d.min_brightness;
                        ev.normalized_brightness = d.normalized_brightness;
                        ev.timestamp = std::chrono::system_clock::now();
                        on_event_(ev);
                    }
                }
            }
            lock.lock();
        }
    }

    std::string target_;
    std::function<void(const BrightnessEvent&)> on_event_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};

class WinBacklightManager final : public BacklightManager {
public:
    WinBacklightManager() = default;
    ~WinBacklightManager() override = default;

    std::vector<BacklightDevice> enumerate_devices() const override {
        return enumerate_win_backlights();
    }

    std::optional<BacklightDevice> find_device_by_name(std::string_view name) const override {
        for (auto& d : enumerate_win_backlights()) {
            if (d.name == name || d.sysfs_path == name) return d;
        }
        return std::nullopt;
    }

    std::optional<BacklightDevice> find_device_for_connector(std::string_view connector_name) const override {
        for (auto& d : enumerate_win_backlights()) {
            if (d.associated_connector == connector_name) return d;
        }
        return std::nullopt;
    }

    std::optional<BacklightDevice> primary_device() const override {
        auto devs = enumerate_win_backlights();
        if (devs.empty()) return std::nullopt;
        // Prefer WMI integrated panel first, then primary connector, then first device.
        for (const auto& d : devs) {
            if (d.type == BacklightType::Firmware) return d;
        }
        return devs.front();
    }

    Result get_brightness(std::string_view device_or_connector,
                          uint32_t& out_raw,
                          double& out_normalized,
                          BrightnessCurve curve) const override {
        auto dev = resolve_device(device_or_connector);
        if (!dev) {
            return Result::failure("No backlight device found for: " + std::string(device_or_connector));
        }
        out_raw = dev->current_brightness;
        out_normalized = dev->to_normalized(out_raw, curve);
        return Result::success();
    }

    Result set_brightness(std::string_view device_or_connector, uint32_t raw_brightness) override {
        auto dev = resolve_device(device_or_connector);
        if (!dev) {
            return Result::failure("No backlight device found for: " + std::string(device_or_connector));
        }
        uint32_t clamped = std::clamp(raw_brightness, dev->min_brightness, dev->max_brightness);
        if (dev->type == BacklightType::Firmware) {
            return set_wmi_backlight(*dev, clamped);
        }
        return set_ddcci_backlight(*dev, clamped);
    }

    Result set_normalized_brightness(std::string_view device_or_connector,
                                     double normalized,
                                     BrightnessCurve curve) override {
        auto dev = resolve_device(device_or_connector);
        if (!dev) {
            return Result::failure("No backlight device found for: " + std::string(device_or_connector));
        }
        uint32_t raw = dev->to_raw(normalized, curve);
        return set_brightness(dev->name, raw);
    }

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::function<void(const BrightnessEvent&)> on_event) override {
        return create_watcher("", std::move(on_event));
    }

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        std::function<void(const BrightnessEvent&)> on_event) override {
        return std::make_unique<WinBacklightWatcher>(std::string(device_or_connector), std::move(on_event));
    }

    std::unique_ptr<BacklightWatcher> create_watcher(
        MessageQueue<BrightnessEvent>& queue) override {
        return create_watcher("", queue);
    }

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        MessageQueue<BrightnessEvent>& queue) override {
        return create_watcher(device_or_connector, [&queue](const BrightnessEvent& ev) {
            queue.push(ev);
        });
    }

    std::unique_ptr<BacklightWatcher> create_watcher(
        MessageQueue<DisplayEvent>& queue) override {
        return create_watcher("", queue);
    }

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        MessageQueue<DisplayEvent>& queue) override {
        return create_watcher(device_or_connector, [&queue](const BrightnessEvent& ev) {
            queue.push(ev);
        });
    }

private:
    std::optional<BacklightDevice> resolve_device(std::string_view device_or_connector) const {
        if (device_or_connector.empty()) {
            return primary_device();
        }
        if (auto d = find_device_by_name(device_or_connector)) return d;
        return find_device_for_connector(device_or_connector);
    }
};

}  // namespace

std::unique_ptr<BacklightManager> BacklightManager::create() {
    return std::make_unique<WinBacklightManager>();
}

}  // namespace brodisplays
