#pragma once

#include "ble_protocol.hpp"
#include <array>
#include <cstdint>

namespace satori::ble {

struct Angles { std::array<float, 3> value{}; };

// Converts the legacy 500..2500 logical input before applying the existing CH3/CH2 coupling.
Angles LogicalTargetToAngles(const Target& target);

class MotionEngine {
public:
    static constexpr std::uint32_t kTickMs = 20;
    void Initialize(const std::array<std::uint16_t, 3>& logical_values);
    void SetTarget(const Target& target, std::uint32_t now_ms, std::uint32_t generation);
    void Halt(std::uint32_t generation);
    void Disconnect(std::uint32_t generation);
    bool Tick(std::uint32_t now_ms, std::uint32_t current_generation);
    const std::array<std::uint16_t, 3>& issued() const { return issued_; }
    std::uint8_t valid_mask() const { return valid_mask_; }
    std::uint8_t interpolating_mask() const { return interpolating_mask_; }
    std::uint32_t last_sequence() const { return last_sequence_; }
    void SetLastSequence(std::uint32_t sequence) { last_sequence_ = sequence; }

private:
    std::array<std::uint16_t, 3> issued_{};
    std::array<std::uint16_t, 3> requested_{};
    std::array<float, 3> from_{}, to_{};
    std::array<std::uint32_t, 3> start_ms_{};
    std::array<std::uint16_t, 3> duration_ms_{};
    std::uint32_t generation_{0}, last_tick_ms_{0}, last_sequence_{0};
    std::uint8_t valid_mask_{0}, interpolating_mask_{0};
    bool generation_known_{false};
};

} // namespace satori::ble
