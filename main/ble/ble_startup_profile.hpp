#pragma once

#include "ble_protocol.hpp"

namespace satori::ble {

// Product profile shipped for the SatoriEye C3 v1 board. Values are logical
// controls in the protocol's 500..2500 range; they are not measured PWM poses.
constexpr Target BuiltInStartupTarget() {
    return Target{{1500, 1500, 1500}, 0};
}

enum class StartupProfileResult { Ok, Invalid };

inline StartupProfileResult SelectStartupTarget(bool custom_confirmed,
                                                bool any_custom_channel,
                                                bool all_custom_channels,
                                                const int custom[3],
                                                Target& out) {
    out = BuiltInStartupTarget();
    if (any_custom_channel && !all_custom_channels) return StartupProfileResult::Invalid;
    if (!custom_confirmed) return StartupProfileResult::Ok;
    if (!all_custom_channels || custom == nullptr) return StartupProfileResult::Invalid;
    for (int i = 0; i < 3; ++i) {
        if (custom[i] < 500 || custom[i] > 2500) return StartupProfileResult::Invalid;
    }
    out.channels = {static_cast<std::uint16_t>(custom[0]),
                    static_cast<std::uint16_t>(custom[1]),
                    static_cast<std::uint16_t>(custom[2])};
    return StartupProfileResult::Ok;
}

} // namespace satori::ble
