#include "ble_protocol.hpp"

namespace satori::ble {
namespace {
std::uint16_t Read16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8)); }
std::uint32_t Read32(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
void Write16(std::uint8_t* p, std::uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
void Write32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xff; }
bool KnownOpcode(std::uint8_t value) { return value >= 1 && value <= 10; }
}

bool Decode(const std::uint8_t* bytes, std::size_t length, Frame& out, DecodeError& error) {
    error = DecodeError::None;
    if (length != kFrameSize || bytes == nullptr) { error = DecodeError::BadLength; return false; }
    if (bytes[0] != kProtocolVersion) { error = DecodeError::BadVersion; return false; }
    if (!KnownOpcode(bytes[1])) { error = DecodeError::BadOpcode; return false; }
    out.version = bytes[0]; out.opcode = bytes[1]; out.sequence = Read32(bytes + 2); out.token = Read32(bytes + 6);
    for (std::size_t i = 0; i < out.payload.size(); ++i) out.payload[i] = bytes[10 + i];
    return true;
}

std::array<std::uint8_t, kFrameSize> Encode(const Frame& frame) {
    std::array<std::uint8_t, kFrameSize> out{};
    out[0] = frame.version; out[1] = frame.opcode; Write32(out.data() + 2, frame.sequence); Write32(out.data() + 6, frame.token);
    for (std::size_t i = 0; i < frame.payload.size(); ++i) out[10 + i] = frame.payload[i];
    return out;
}

std::array<std::uint8_t, kFrameSize> EncodeReply(const Frame& request, Result result, const Snapshot& state,
                                                std::uint32_t response_token, bool bonded) {
    Frame reply; reply.opcode = static_cast<std::uint8_t>(request.opcode | 0x80); reply.sequence = request.sequence; reply.token = response_token;
    reply.payload[0] = static_cast<std::uint8_t>(result); reply.payload[1] = static_cast<std::uint8_t>(state.state);
    Write32(reply.payload.data() + 2, state.last_applied_sequence);
    reply.payload[6] = static_cast<std::uint8_t>((state.valid_mask ? 1 : 0) | (state.interpolating_mask ? 2 : 0) | (bonded ? 4 : 0) | (state.token ? 8 : 0));
    reply.payload[7] = state.battery_percent;
    return Encode(reply);
}

std::array<std::uint8_t, kFrameSize> EncodeDeviceInfo(const DeviceInfo& info) {
    std::array<std::uint8_t, kFrameSize> out{};
    out[0] = 1; out[1] = info.protocol_minor; out[2] = info.firmware_major; out[3] = info.firmware_minor; out[4] = info.firmware_patch; out[5] = info.hardware_profile;
    Write32(out.data() + 6, info.capabilities); out[10] = kTargetHz; out[11] = kTargetHz;
    Write16(out.data() + 12, kMaxTransitionMs); Write16(out.data() + 14, kLeaseTimeoutMs);
    out[16] = info.security_policy; out[17] = 3;
    return out;
}

std::array<std::uint8_t, 16> EncodeIdentity(const std::array<std::uint8_t, 16>& identity) { return identity; }

std::array<std::uint8_t, kFrameSize> EncodeSnapshot(const Snapshot& state) {
    std::array<std::uint8_t, kFrameSize> out{};
    out[0] = 1; out[1] = static_cast<std::uint8_t>(state.state); Write32(out.data() + 2, state.token); Write32(out.data() + 6, state.last_applied_sequence);
    for (int i = 0; i < 3; ++i) Write16(out.data() + 10 + i * 2, (state.valid_mask & (1 << i)) ? state.channels[i] : 0);
    out[16] = state.valid_mask & 7; out[17] = state.interpolating_mask & 7; out[18] = state.battery_percent;
    return out;
}

bool PayloadIsZero(const Frame& frame) { for (auto byte : frame.payload) if (byte != 0) return false; return true; }

bool DecodeTarget(const Frame& frame, Target& target, Result& error) {
    error = Result::BadPayload;
    for (int i = 0; i < 3; ++i) target.channels[i] = Read16(frame.payload.data() + i * 2);
    target.transition_ms = Read16(frame.payload.data() + 6);
    if (Read16(frame.payload.data() + 8) != 0 || target.transition_ms > kMaxTransitionMs) return false;
    for (auto channel : target.channels) if (channel < 500 || channel > 2500) return false;
    error = Result::Ok;
    return true;
}

bool DecodePairingCode(const Frame& frame, std::uint32_t& code, Result& error) {
    error = Result::BadPayload;
    if (frame.opcode != static_cast<std::uint8_t>(Opcode::SetPairingCode)) return false;
    for (std::size_t i = 4; i < frame.payload.size(); ++i) if (frame.payload[i] != 0) return false;
    code = Read32(frame.payload.data());
    if (code > 999999u || code == kBleDefaultPairingCode) return false;
    error = Result::Ok;
    return true;
}
} // namespace satori::ble
