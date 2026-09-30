#include "ble_pairing.hpp"

#include <cstring>

namespace satori::ble {

bool PeerIdentity::operator==(const PeerIdentity& other) const {
    return address_type == other.address_type && address == other.address;
}

PairingCore::PairingCore() = default;

PairingCore::PairingCore(const char* persisted_code) {
    if (ValidCode(persisted_code)) std::memcpy(pairing_code_.data(), persisted_code, pairing_code_.size());
}

bool PairingCore::ValidCode(const char* code) {
    if (code == nullptr) return false;
    for (std::size_t i = 0; i < 6; ++i) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    return code[6] == '\0';
}

bool PairingCore::Contains(const PeerIdentity& peer) const {
    for (std::size_t i = 0; i < bonded_phone_count_; ++i) {
        if (bonded_phones_[i] == peer) return true;
    }
    return false;
}

bool PairingCore::IsAuthorized(const PeerIdentity& peer, const LinkSecurity& security) const {
    return security.secure() && Contains(peer);
}

PairingResult PairingCore::RestoreBond(const PeerIdentity& peer) {
    if (Contains(peer)) return PairingResult::Ok;
    if (bonded_phone_count_ >= bonded_phones_.size()) return PairingResult::BondLimitReached;
    bonded_phones_[bonded_phone_count_++] = peer;
    return PairingResult::Ok;
}

PairingResult PairingCore::RegisterSecureBond(const PeerIdentity& peer, const LinkSecurity& security) {
    if (!security.secure()) return PairingResult::NotAuthorized;
    return RestoreBond(peer);
}

PairingResult PairingCore::ValidateCodeChange(const PeerIdentity& peer, const LinkSecurity& security,
                                               const char* new_code) {
    has_staged_code_ = false;
    if (!IsAuthorized(peer, security)) return PairingResult::NotAuthorized;
    if (!ValidCode(new_code)) return PairingResult::InvalidCode;
    if (std::memcmp(new_code, kDefaultPairingCode, 6) == 0) return PairingResult::DefaultCodeForbidden;
    std::memcpy(staged_code_.data(), new_code, staged_code_.size());
    has_staged_code_ = true;
    return PairingResult::Ok;
}

PairingResult PairingCore::CommitCodeChange() {
    if (!has_staged_code_) return PairingResult::InvalidCode;
    pairing_code_ = staged_code_;
    has_staged_code_ = false;
    return PairingResult::Ok;
}

void PairingCore::AbortCodeChange() {
    has_staged_code_ = false;
}

} // namespace satori::ble
