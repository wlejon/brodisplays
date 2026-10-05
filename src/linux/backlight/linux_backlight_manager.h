#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "brodisplays/backlight.h"
#include "logind_brightness.h"

namespace brodisplays {

class LinuxBacklightManager : public BacklightManager {
public:
    LinuxBacklightManager();
    ~LinuxBacklightManager() override;

    std::vector<BacklightDevice> enumerate_devices() const override;
    std::optional<BacklightDevice> find_device_by_name(std::string_view name) const override;
    std::optional<BacklightDevice> find_device_for_connector(std::string_view connector_name) const override;
    std::optional<BacklightDevice> primary_device() const override;

    Result get_brightness(std::string_view device_or_connector,
                          uint32_t& out_raw,
                          double& out_normalized,
                          BrightnessCurve curve = BrightnessCurve::Perceptual) const override;

    Result set_brightness(std::string_view device_or_connector, uint32_t raw_brightness) override;

    Result set_normalized_brightness(std::string_view device_or_connector,
                                     double normalized,
                                     BrightnessCurve curve = BrightnessCurve::Perceptual) override;

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::function<void(const BrightnessEvent&)> on_event) override;

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        std::function<void(const BrightnessEvent&)> on_event) override;

    std::unique_ptr<BacklightWatcher> create_watcher(
        MessageQueue<BrightnessEvent>& queue) override;

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        MessageQueue<BrightnessEvent>& queue) override;

    std::unique_ptr<BacklightWatcher> create_watcher(
        MessageQueue<DisplayEvent>& queue) override;

    std::unique_ptr<BacklightWatcher> create_watcher(
        std::string_view device_or_connector,
        MessageQueue<DisplayEvent>& queue) override;

private:
    std::optional<BacklightDevice> resolve_device(std::string_view device_or_connector) const;

    mutable LogindBrightnessClient logind_client_;
};

} // namespace brodisplays
