#include "ble_startup_profile.hpp"
#include "ble_motion.hpp"
#include "servor_input_adapter.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace satori::ble;

int main() {
    Target selected{};
    const int absent[3] = {1500, 1500, 1500};
    assert(SelectStartupTarget(false, false, false, absent, selected) == StartupProfileResult::Ok);
    assert((selected.channels == std::array<std::uint16_t, 3>{1500, 1500, 1500}));

    const int custom[3] = {1300, 1500, 1700};
    assert(SelectStartupTarget(true, true, true, custom, selected) == StartupProfileResult::Ok);
    assert((selected.channels == std::array<std::uint16_t, 3>{1300, 1500, 1700}));
    assert(SelectStartupTarget(true, true, false, custom, selected) == StartupProfileResult::Invalid);
    const int invalid[3] = {499, 1500, 1700};
    assert(SelectStartupTarget(true, true, true, invalid, selected) == StartupProfileResult::Invalid);

    const auto logical = LogicalTargetToAngles(BuiltInStartupTarget());
    assert(std::fabs(logical.value[0] - 90.0f) < 0.001f);
    assert(std::fabs(logical.value[1] - 90.0f) < 0.001f);
    assert(std::fabs(logical.value[2] - 90.0f) < 0.001f);

    const ServoDataConfig ch1{0.333f, 0.0f, 90.0f, 45.0f, 135.0f, false};
    const ServoDataConfig ch2{0.333f, 40.0f, 60.0f, 0.0f, 90.0f, true};
    const ServoDataConfig ch3{0.333f, -30.0f, 120.0f, 120.0f, 180.0f, true};
    assert(std::fabs(CalibratedServoAngle(logical.value[0], ch1) - 90.0f) < 0.01f);
    assert(std::fabs(CalibratedServoAngle(logical.value[1], ch2) - 30.0f) < 0.02f);
    assert(std::fabs(CalibratedServoAngle(logical.value[2], ch3) - 140.0f) < 0.02f);
    std::cout << "satori_c3_v1 logical startup and configured mechanical mapping: passed\n";
}
