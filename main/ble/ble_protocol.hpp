#pragma once

#include <array>
#include <cstdint>

namespace satori::ble {

constexpr std::size_t kFrameSize = 20;
constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::uint16_t kMaxTransitionMs = 2000;
constexpr std::uint16_t kLeaseTimeoutMs = 6000;
constexpr std::uint8_t kTargetHz = 20;
constexpr std::uint32_t kBleDefaultPairingCode = 123456;
constexpr std::uint32_t kCapabilityPairingCodeManagement = 1u << 7;
constexpr std::uint32_t kCapabilitySharedMultiBond = 1u << 8;

enum class Opcode : std::uint8_t {
    Claim = 0x01, SetTarget = 0x02, Halt = 0x03, Release = 0x04,
    Keepalive = 0x05, GetStatus = 0x06, Arm = 0x07, AsyncState = 0xE0,
    SetPairingCode = 0x08, OpenTransfer = 0x09, CancelTransfer = 0x0A,
};
enum class Result : std::uint8_t {
    Ok = 0, BadVersion = 1, BadLength = 2, BadOpcode = 3,
    BadPayload = 4, NotAuthorized = 5, BadSession = 6,
    OldSequence = 7, SequenceConflict = 8, Busy = 9,
    InternalError = 10, NotArmed = 11, NotConfigured = 12,
    SubscriptionRequired = 13,
};
enum class ControlState : std::uint8_t { Unclaimed = 0, Holding = 1, Interpolating = 2, Fault = 3 };

struct Frame {
    std::uint8_t version{kProtocolVersion};
    std::uint8_t opcode{0};
    std::uint32_t sequence{0};
    std::uint32_t token{0};
    std::array<std::uint8_t, 10> payload{};
};

struct DeviceInfo {
    std::uint8_t firmware_major{0}, firmware_minor{0}, firmware_patch{0};
    std::uint8_t protocol_minor{0};
    std::uint8_t hardware_profile{1};
    std::uint32_t capabilities{0x5f};
    std::uint8_t security_policy{1};
};

struct Snapshot {
    ControlState state{ControlState::Unclaimed};
    std::uint32_t token{0}, last_applied_sequence{0};
    std::array<std::uint16_t, 3> channels{};
    std::uint8_t valid_mask{0}, interpolating_mask{0}, battery_percent{255};
};

struct Target {
    std::array<std::uint16_t, 3> channels{};
    std::uint16_t transition_ms{0};
};

enum class DecodeError { None, BadLength, BadVersion, BadOpcode };

bool Decode(const std::uint8_t* bytes, std::size_t length, Frame& out, DecodeError& error);
std::array<std::uint8_t, kFrameSize> Encode(const Frame& frame);
std::array<std::uint8_t, kFrameSize> EncodeReply(const Frame& request, Result result, const Snapshot& state,
                                                std::uint32_t response_token, bool bonded);
std::array<std::uint8_t, kFrameSize> EncodeDeviceInfo(const DeviceInfo& info);
std::array<std::uint8_t, 16> EncodeIdentity(const std::array<std::uint8_t, 16>& identity);
std::array<std::uint8_t, kFrameSize> EncodeSnapshot(const Snapshot& state);
bool DecodeTarget(const Frame& frame, Target& target, Result& error);
bool DecodePairingCode(const Frame& frame, std::uint32_t& code, Result& error);
bool PayloadIsZero(const Frame& frame);

} // namespace satori::ble
