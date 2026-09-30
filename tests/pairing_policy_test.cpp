#include "../main/ble/ble_pairing.hpp"
#include <cassert>

using namespace satori::ble;

static PeerIdentity Peer(std::uint8_t n) {
    PeerIdentity p{}; p.address_type = 1; p.address = {{n, 1, 2, 3, 4, 5}}; return p;
}

int main() {
    const LinkSecurity sc{true, true, true};
    const LinkSecurity no_mitm{true, false, true};
    PairingCore policy;

    // Eight bonded phones are retained, and the ninth is rejected without eviction.
    for (std::uint8_t i = 0; i < 8; ++i)
        assert(policy.RegisterSecureBond(Peer(i), sc) == PairingResult::Ok);
    assert(policy.bonded_phone_count() == 8);
    assert(policy.RegisterSecureBond(Peer(8), sc) == PairingResult::BondLimitReached);
    for (std::uint8_t i = 0; i < 8; ++i) assert(policy.IsAuthorized(Peer(i), sc));
    assert(!policy.IsAuthorized(Peer(8), sc));

    // A failed bond write must not register the candidate in policy memory.
    PairingCore failed_write;
    assert(failed_write.RegisterSecureBond(Peer(40), no_mitm) == PairingResult::NotAuthorized);
    assert(failed_write.bonded_phone_count() == 0);
    assert(!failed_write.IsAuthorized(Peer(40), sc));

    // Shared code changes can be made by any currently persisted SC peer.
    assert(policy.ValidateCodeChange(Peer(3), sc, "246810") == PairingResult::Ok);
    assert(policy.CommitCodeChange() == PairingResult::Ok);
    assert(policy.ValidateCodeChange(Peer(7), sc, "135790") == PairingResult::Ok);
    policy.AbortCodeChange(); // model NVS failure: prior code remains in effect
    assert(policy.ValidateCodeChange(Peer(6), sc, "123456") == PairingResult::DefaultCodeForbidden);
    assert(policy.ValidateCodeChange(Peer(6), no_mitm, "135790") == PairingResult::NotAuthorized);
    assert(policy.ValidateCodeChange(Peer(9), sc, "135790") == PairingResult::NotAuthorized);

    // Restoring the persisted bond list is idempotent and preserves legacy bonds.
    assert(policy.RestoreBond(Peer(0)) == PairingResult::Ok);
    assert(policy.bonded_phone_count() == 8);
}
