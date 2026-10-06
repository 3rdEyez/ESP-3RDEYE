#include "sdkconfig.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server_internal.hpp"
#include "ble_startup_profile.hpp"
#include "esp_partition_param.h"
#include "driver/gpio.h"
#include <cmath>
#include <string>

namespace satori::ble::internal {
bool IsStartupConfigurationValid(Target& startup_target) {
    auto& config = EspPartitionParam::GetInstance();
    if (!config.IsValid()) return false;
    const bool custom_confirmed = config.GetBoolParam("BLE_STARTUP_CONFIRMED", false);
    constexpr const char* startup_keys[] = {"BLE_STARTUP_CH1", "BLE_STARTUP_CH2", "BLE_STARTUP_CH3"};
    int custom[3] = {1500, 1500, 1500};
    bool any_custom = false, all_custom = true;
    for (int i = 0; i < 3; ++i) {
        const bool present = config.HasParam(startup_keys[i]);
        any_custom |= present;
        all_custom &= present;
        if (present) {
            custom[i] = config.GetIntParam(startup_keys[i], -1);
            if (custom[i] < 500 || custom[i] > 2500) return false;
        }
    }
    Target startup{};
    if (SelectStartupTarget(custom_confirmed, any_custom, all_custom, custom, startup) != StartupProfileResult::Ok)
        return false;
    const int pins[3] = {
        config.GetIntParam("SERVO_PULSE_GPIO_CH1", CONFIG_SERVO_PULSE_GPIO_CH1),
        config.GetIntParam("SERVO_PULSE_GPIO_CH2", CONFIG_SERVO_PULSE_GPIO_CH2),
        config.GetIntParam("SERVO_PULSE_GPIO_CH3", CONFIG_SERVO_PULSE_GPIO_CH3),
    };
    const float default_scale[3] = {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
    const float default_offset[3] = {0.0f, 40.0f, -40.0f};
    const float default_zero[3] = {90.0f, 60.0f, 120.0f};
    const float default_min[3] = {45.0f, 0.0f, 120.0f};
    const float default_max[3] = {135.0f, 90.0f, 180.0f};
    const bool default_reverse[3] = {true, true, true};
    for (int i = 0; i < 3; ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return false;
        for (int j = 0; j < i; ++j) if (pins[i] == pins[j]) return false;
        const std::string suffix = std::to_string(i + 1);
        const float scale = config.GetFloatParam("SERVO_SCALE_CH" + suffix, default_scale[i]);
        const float offset = config.GetFloatParam("SERVO_OFFSET_CH" + suffix, default_offset[i]);
        const float zero = config.GetFloatParam("SERVO_ZEROPOINT_CH" + suffix, default_zero[i]);
        const float min_angle = config.GetFloatParam("SERVO_MIN_ANGLE_CH" + suffix, default_min[i]);
        const float max_angle = config.GetFloatParam("SERVO_MAX_ANGLE_CH" + suffix, default_max[i]);
        (void)config.GetBoolParam("SERVO_IS_REVERSE_CH" + suffix, default_reverse[i]);
        if (!std::isfinite(scale) || !std::isfinite(offset) || !std::isfinite(zero) || !std::isfinite(min_angle) || !std::isfinite(max_angle) ||
            scale <= 0.0f || min_angle < 0.0f || max_angle > 180.0f || min_angle >= max_angle || zero < 0.0f || zero > 180.0f) return false;
    }
    startup_target = startup;
    return config.IsValid();
}

} // namespace satori::ble::internal
#endif
