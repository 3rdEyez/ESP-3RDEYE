#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace satori::ble {

// NimBLE remains the durable bond store. This value object mirrors its bounded
// contents so policy and code-change authorization can be tested on the host.
struct PeerIdentity {
    std::uint8_t address_type{0};
    std::array<std::uint8_t, 6> address{};
    bool operator==(const PeerIdentity& other) const;
    bool operator!=(const PeerIdentity& other) const { return !(*this == other); }
};

struct LinkSecurity {
    bool encrypted{false};
    bool authenticated{false};
    bool bonded{false};
    bool secure() const { return encrypted && authenticated && bonded; }
};

enum class PairingResult : std::uint8_t {
    Ok, NotAuthorized, InvalidCode, DefaultCodeForbidden, BondLimitReached
};

class PairingCore {
public:
    static constexpr std::size_t kMaxBondedPhones = 8;
    static constexpr const char* kDefaultPairingCode = "123456";

    PairingCore();
    explicit PairingCore(const char* persisted_code);

    const char* pairing_code() const { return pairing_code_.data(); }
    std::size_t bonded_phone_count() const { return bonded_phone_count_; }
    const PeerIdentity* bonded_phone(std::size_t index) const {
        return index < bonded_phone_count_ ? &bonded_phones_[index] : nullptr;
    }

    bool CanBeginPairing(std::size_t durable_bond_count) const {
        return durable_bond_count < kMaxBondedPhones;
    }
    bool IsAuthorized(const PeerIdentity& peer, const LinkSecurity& security) const;
    // Existing NimBLE bond-store entries are trusted during startup restore.
    PairingResult RestoreBond(const PeerIdentity& peer);
    // New entries are admitted only after authenticated, encrypted SC bonding.
    PairingResult RegisterSecureBond(const PeerIdentity& peer, const LinkSecurity& security);
    PairingResult ValidateCodeChange(const PeerIdentity& peer, const LinkSecurity& security,
                                     const char* new_code);
    // Call only after the staged code has been durably committed to NVS.
    PairingResult CommitCodeChange();
    void AbortCodeChange();

private:
    static bool ValidCode(const char* code);
    bool Contains(const PeerIdentity& peer) const;

    std::array<PeerIdentity, kMaxBondedPhones> bonded_phones_{};
    std::array<char, 7> pairing_code_{{'1','2','3','4','5','6','\0'}};
    std::array<char, 7> staged_code_{};
    std::size_t bonded_phone_count_{0};
    bool has_staged_code_{false};
};

} // namespace satori::ble
