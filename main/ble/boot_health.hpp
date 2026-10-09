#pragma once
#include <cstdint>

namespace satori::ble {
enum class BootDecision { Wait, Confirm, Fail };
// ESP-IDF OTA state values. Callers assert their SDK values match before use.
enum class BootImageState : std::uint32_t {
    New = 0, PendingVerify = 1, Valid = 2, Invalid = 3, Aborted = 4,
    Undefined = 0xffffffffu,
};
constexpr bool BootImageCanValidate(std::uint32_t state) {
    return state == static_cast<std::uint32_t>(BootImageState::Valid) ||
           state == static_cast<std::uint32_t>(BootImageState::PendingVerify);
}
// Observes existing service health; never initializes or drives an output.
class BootHealthWindow {
public:
    BootDecision Observe(std::uint32_t elapsed_ms, bool healthy, bool control_advanced) {
        if (elapsed_ms >= 15000) return BootDecision::Fail;
        if (!healthy || !control_advanced) consecutive_ = 0;
        else if (++consecutive_ >= 5) return BootDecision::Confirm;
        return BootDecision::Wait;
    }
private:
    std::uint8_t consecutive_{0};
};
}
