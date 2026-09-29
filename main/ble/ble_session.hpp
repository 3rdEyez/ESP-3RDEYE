#pragma once

#include "ble_protocol.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace satori::ble {

struct Peer {
    std::array<std::uint8_t, 6> address{};
    bool encrypted{false}, authenticated{false}, bonded{false};
};
struct Outcome {
    Result result{Result::Ok};
    Frame request{};
    Target target{};
    bool has_reply{true}, duplicate{false}, accepted{false};
    bool action_required{false}, disconnect{false};
    std::uint32_t generation{0};
    std::array<std::uint8_t, kFrameSize> reply{};
};
class Session {
public:
    void Connect(const Peer& peer, bool event_subscribed, std::uint32_t now_ms);
    void SetSubscribed(bool subscribed);
    void Disconnect();
    Outcome Handle(const std::uint8_t* bytes, std::size_t length, std::uint32_t now_ms,
                   bool startup_configured, const Target& startup_target, std::uint32_t claim_token);
    bool TickLease(std::uint32_t now_ms);
    bool CompleteAction(std::uint32_t generation, std::uint32_t sequence, bool success,
                        std::array<std::uint8_t, kFrameSize>& reply);
    bool FailAction(std::uint32_t generation, std::uint32_t sequence, Result result,
                    std::array<std::uint8_t, kFrameSize>& reply);
    void UpdateMotion(const std::array<std::uint16_t, 3>& issued, std::uint8_t interpolating_mask,
                      std::uint32_t applied_sequence);
    const Snapshot& snapshot() const { return snapshot_; }
    std::uint32_t generation() const { return generation_; }
    bool connected() const { return connected_; }

private:
    struct CacheEntry { bool used{false}; std::uint32_t sequence{0}; std::array<std::uint8_t, kFrameSize> request{}, reply{}; };
    void ClearSession(bool clear_cache);
    Outcome Reject(const Frame& request, Result result) const;
    void Cache(const Frame& request, const std::array<std::uint8_t, kFrameSize>& reply);
    bool Authorized() const;

    Peer peer_{};
    Snapshot snapshot_{};
    bool connected_{false}, subscribed_{false}, has_owner_{false}, released_{false};
    std::uint32_t last_valid_ms_{0}, highest_sequence_{0}, release_deadline_ms_{0}, generation_{1};
    std::array<CacheEntry, 16> cache_{};
    std::size_t cache_cursor_{0};
    Frame pending_action_{};
    bool pending_action_valid_{false};
    Opcode pending_opcode_{Opcode::GetStatus};
    Target pending_target_{};
};

} // namespace satori::ble
