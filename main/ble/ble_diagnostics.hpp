#pragma once

#include <array>
#include <cstdint>

namespace satori::ble {
// Optional, authenticated read-only characteristic; the v1.2 control frames
// and safety state machine are unchanged. These are observations, not commands.
enum class StopReason : std::uint8_t {
    None = 0, Halt = 1, Release = 2, LinkLost = 3, LeaseExpired = 4,
    OutputFault = 5, StorageFault = 6, QueueFault = 7, HostReset = 8,
};
enum DiagnosticFault : std::uint8_t {
    PwmFault = 1, BondStorageFault = 2, WorkQueueFault = 4,
    IdentityFault = 8, StartupConfigurationFault = 16,
};
struct Diagnostics {
    std::uint8_t reset_reason{0};
    StopReason last_stop{StopReason::None};
    std::uint8_t faults{0};
    std::uint32_t uptime_seconds{0}, disconnect_count{0};
    std::uint16_t lease_expiry_count{0}, notification_failure_count{0}, last_gap_reason{0};
};
inline void IncrementDiagnosticCounter(std::uint16_t& value) {
    if (value != UINT16_MAX) ++value;
}
inline std::array<std::uint8_t, 20> EncodeDiagnostics(const Diagnostics& value) {
    std::array<std::uint8_t, 20> out{};
    out[0] = 1; out[1] = value.reset_reason;
    out[2] = static_cast<std::uint8_t>(value.last_stop); out[3] = value.faults;
    const auto put32 = [&](int offset, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out[offset + i] = (v >> (8 * i)) & 0xff;
    };
    const auto put16 = [&](int offset, std::uint16_t v) {
        out[offset] = v & 0xff; out[offset + 1] = v >> 8;
    };
    put32(4, value.uptime_seconds); put32(8, value.disconnect_count);
    put16(12, value.lease_expiry_count); put16(14, value.notification_failure_count);
    put16(16, value.last_gap_reason);
    return out;
}
} // namespace satori::ble
