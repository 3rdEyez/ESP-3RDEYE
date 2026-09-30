#include "../../main/ble/ble_pairing.hpp"
#include <cassert>
#include <cstring>

using namespace satori::ble;

static PeerIdentity Id(std::uint8_t first) {
    PeerIdentity id{};
    id.address_type = 1;
    id.address = {{first, 2, 3, 4, 5, 6}};
    return id;
}

int main() {
    const LinkSecurity secure{true, true, true};
    const LinkSecurity non_sc{true, false, true};
    PairingCore core;
    assert(std::strcmp(core.pairing_code(), "123456") == 0);

    // Existing durable phones restore without deleting or replacing one another.
    for (std::uint8_t i = 1; i <= 8; ++i) assert(core.RestoreBond(Id(i)) == PairingResult::Ok);
    assert(core.bonded_phone_count() == 8);
    for (std::uint8_t i = 1; i <= 8; ++i) assert(core.IsAuthorized(Id(i), secure));
    assert(!core.IsAuthorized(Id(1), non_sc));
    assert(core.CanBeginPairing(7));
    assert(!core.CanBeginPairing(8));
    assert(core.RestoreBond(Id(9)) == PairingResult::BondLimitReached);
    assert(core.IsAuthorized(Id(1), secure) && core.IsAuthorized(Id(8), secure));

    // New entries require authenticated SC; duplicate persisted bonds are idempotent.
    PairingCore partial;
    assert(partial.RegisterSecureBond(Id(1), non_sc) == PairingResult::NotAuthorized);
    assert(partial.bonded_phone_count() == 0);
    for (std::uint8_t i = 1; i <= 8; ++i)
        assert(partial.RegisterSecureBond(Id(i), secure) == PairingResult::Ok);
    assert(partial.RegisterSecureBond(Id(4), secure) == PairingResult::Ok);
    assert(partial.RegisterSecureBond(Id(9), secure) == PairingResult::BondLimitReached);
    assert(partial.bonded_phone_count() == 8);
    for (std::uint8_t i = 1; i <= 8; ++i) assert(partial.IsAuthorized(Id(i), secure));

    assert(partial.ValidateCodeChange(Id(9), secure, "654321") == PairingResult::NotAuthorized);
    assert(partial.ValidateCodeChange(Id(1), non_sc, "654321") == PairingResult::NotAuthorized);
    assert(partial.ValidateCodeChange(Id(2), secure, "123456") == PairingResult::DefaultCodeForbidden);
    assert(partial.ValidateCodeChange(Id(2), secure, "12x456") == PairingResult::InvalidCode);
    assert(partial.ValidateCodeChange(Id(2), secure, "654321") == PairingResult::Ok);
    partial.AbortCodeChange(); // failed NVS commit preserves the prior shared code
    assert(std::strcmp(partial.pairing_code(), "123456") == 0);
    assert(partial.ValidateCodeChange(Id(7), secure, "654321") == PairingResult::Ok);
    assert(partial.CommitCodeChange() == PairingResult::Ok);
    assert(std::strcmp(partial.pairing_code(), "654321") == 0);
    for (std::uint8_t i = 1; i <= 8; ++i) assert(partial.IsAuthorized(Id(i), secure));
}
