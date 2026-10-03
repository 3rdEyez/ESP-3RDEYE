#pragma once
#include <cstdint>

namespace satori::ble {
// Caller acquires each atomic snapshot in order. An ACK refers only to the
// epoch whose work was halted; changing epoch invalidates all older ACKs.
// Epoch zero is uninitialized (or a counter wrap): fail closed for that window.
constexpr bool MaintenanceStopAckMatches(bool requested, std::uint32_t epoch_before,
                                        std::uint32_t stopped_epoch, std::uint32_t epoch_after) {
    return requested && epoch_before != 0 && epoch_before == epoch_after && stopped_epoch == epoch_before;
}
} // namespace satori::ble
