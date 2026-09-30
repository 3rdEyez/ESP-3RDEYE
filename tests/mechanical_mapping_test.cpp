#include "servor_input_adapter.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    const auto close = [](float a, float b) { return std::abs(a - b) < 0.0001f; };
    ServoInputAdapter logical;
    assert(close(logical.PulseWidth2Angle(500), 0));
    assert(close(logical.PulseWidth2Angle(1500), 90));
    assert(close(logical.PulseWidth2Angle(2500), 180));
    // Historical calibration defaults, deliberately distinct zero/offset/reversal.
    const ServoDataConfig c1{1.0f / 3, 0, 90, 45, 135, true};
    const ServoDataConfig c2{1.0f / 3, 40, 60, 0, 90, true};
    const ServoDataConfig c3{1.0f / 3, -40, 120, 120, 180, true};
    assert(close(CalibratedServoAngle(90, c1), 90));
    assert(close(CalibratedServoAngle(99, c2), 27));
    assert(close(CalibratedServoAngle(100.8f, c3), 146.4f));
    // Clamp must happen after offset/scale, including coupled values outside 0..180.
    assert(close(CalibratedServoAngle(400, c2), 0));
    assert(close(CalibratedServoAngle(-400, c3), 180));
    const ServoDataConfig asymmetric{0.5f, 10, 50, 0, 180, false};
    assert(close(CalibratedServoAngle(90, asymmetric), 60));
    std::cout << "Mechanical logical mapping and calibration order passed\n";
}
