#include "ble_motion.hpp"
#include <algorithm>
#include <cmath>

namespace satori::ble {
namespace {
float ToAngle(std::uint16_t pulse) { return (static_cast<float>(pulse) - 500.0f) / 2000.0f * 180.0f; }
std::uint16_t FromAngle(float angle) {
    const float pulse = angle / 180.0f * 2000.0f + 500.0f;
    return static_cast<std::uint16_t>(std::clamp(std::lround(pulse), 500l, 2500l));
}
}

Angles LogicalTargetToAngles(const Target& target) {
    Angles result{{ToAngle(target.channels[0]), ToAngle(target.channels[1]), ToAngle(target.channels[2])}};
    result.value[2] -= (result.value[1] - 90.0f) * 0.8f;
    return result;
}

void MotionEngine::Initialize(const std::array<std::uint16_t, 3>& logical_values) {
    issued_ = requested_ = logical_values; valid_mask_ = 7; interpolating_mask_ = 0;
    generation_known_ = false;
    duration_ms_ = {};
    for (int i = 0; i < 3; ++i) from_[i] = to_[i] = ToAngle(logical_values[i]);
}
void MotionEngine::SetTarget(const Target& target, std::uint32_t now_ms, std::uint32_t generation) {
    // Reject callbacks queued before a HALT/RELEASE/disconnect generation barrier.
    if (generation_known_ && generation != generation_) return;
    const auto mapped = LogicalTargetToAngles(target);
    if (target.transition_ms > kMaxTransitionMs ||
        !std::all_of(target.channels.begin(), target.channels.end(), [](std::uint16_t value) { return value >= 500 && value <= 2500; }) ||
        !std::all_of(mapped.value.begin(), mapped.value.end(), [](float value) { return std::isfinite(value); })) return;
    // Interpolate the three raw logical controls; mechanical CH3/CH2 coupling is applied
    // exactly once by LogicalTargetToAngles at the hardware output boundary.
    const bool already_interpolating = interpolating_mask_ != 0;
    for (int i = 0; i < 3; ++i) {
        if (target.channels[i] == requested_[i]) continue;
        requested_[i] = target.channels[i];
        from_[i] = ToAngle(issued_[i]);
        to_[i] = ToAngle(target.channels[i]);
        duration_ms_[i] = target.transition_ms;
        start_ms_[i] = now_ms;
        if (duration_ms_[i] == 0) {
            issued_[i] = target.channels[i];
            from_[i] = to_[i];
            interpolating_mask_ &= static_cast<std::uint8_t>(~(1u << i));
        } else if (issued_[i] == target.channels[i]) {
            from_[i] = to_[i];
            duration_ms_[i] = 0;
            interpolating_mask_ &= static_cast<std::uint8_t>(~(1u << i));
        } else {
            interpolating_mask_ |= static_cast<std::uint8_t>(1u << i);
        }
    }
    if (!already_interpolating && interpolating_mask_ != 0) last_tick_ms_ = now_ms;
    generation_ = generation;
    generation_known_ = true;
    valid_mask_ = 7;
}
void MotionEngine::Halt(std::uint32_t generation) {
    generation_ = generation; generation_known_ = true; interpolating_mask_ = 0;
    for (int i = 0; i < 3; ++i) {
        requested_[i] = issued_[i];
        from_[i] = to_[i] = ToAngle(issued_[i]);
        duration_ms_[i] = 0;
    }
}
void MotionEngine::Disconnect(std::uint32_t generation) { Halt(generation); }
bool MotionEngine::Tick(std::uint32_t now_ms, std::uint32_t current_generation) {
    if (current_generation != generation_ || interpolating_mask_ == 0 || now_ms == last_tick_ms_) return false;
    if (static_cast<std::uint32_t>(now_ms - last_tick_ms_) < kTickMs) return false;
    last_tick_ms_ = now_ms;
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        const auto bit = static_cast<std::uint8_t>(1u << i);
        if ((interpolating_mask_ & bit) == 0) continue;
        const auto duration = duration_ms_[i];
        if (duration == 0) { interpolating_mask_ &= static_cast<std::uint8_t>(~bit); continue; }
        const auto elapsed = static_cast<std::uint32_t>(now_ms - start_ms_[i]);
        const float t = std::min(1.0f, static_cast<float>(elapsed) / duration);
        const float smooth = t * t * t * (10.0f + t * (-15.0f + 6.0f * t));
        const float angle = from_[i] + (to_[i] - from_[i]) * smooth;
        if (!std::isfinite(angle)) {
            requested_[i] = issued_[i];
            from_[i] = to_[i] = ToAngle(issued_[i]);
            duration_ms_[i] = 0;
            interpolating_mask_ &= static_cast<std::uint8_t>(~bit);
            continue;
        }
        issued_[i] = t >= 1.0f ? requested_[i] : FromAngle(angle);
        changed = true;
        if (t >= 1.0f) {
            from_[i] = to_[i];
            duration_ms_[i] = 0;
            interpolating_mask_ &= static_cast<std::uint8_t>(~bit);
        }
    }
    return changed;
}

} // namespace satori::ble
