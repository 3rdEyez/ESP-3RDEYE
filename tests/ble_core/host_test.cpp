#include "vector_fixture.hpp"
#include "../../main/ble/ble_protocol.hpp"
#include "../../main/ble/ble_session.hpp"
#include "../../main/ble/ble_motion.hpp"
#include "../../main/ble/ble_startup_profile.hpp"
#include "../../main/ble/ble_diagnostics.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace satori::ble;

static std::vector<std::uint8_t> Bytes(const char* hex) {
    const std::string text(hex);
    assert(text.size() % 2 == 0);
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = digit(text[i]), lo = digit(text[i + 1]);
        assert(hi >= 0 && lo >= 0);
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

static std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8)); }
static std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
static void P16(std::uint8_t* p, std::uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }

static std::array<std::uint8_t, 20> Fixed(const std::vector<std::uint8_t>& bytes) {
    assert(bytes.size() == 20);
    std::array<std::uint8_t, 20> result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

static std::array<std::uint8_t, 20> Command(const char* hex) { return Fixed(Bytes(hex)); }

static Result Expected(const char* text);

static Peer SecurePeer() {
    Peer p{}; p.encrypted = p.authenticated = p.bonded = true; return p;
}

static void Claim(Session& session, bool subscribed = true, std::uint32_t now = 0, std::uint32_t token = 0x12345678) {
    session.Connect(SecurePeer(), subscribed, now);
    const auto bytes = Command("0101010000000000000000000000000000000000");
    const auto result = session.Handle(bytes.data(), bytes.size(), now, false, {}, token);
    assert(result.result == Result::Ok && result.accepted);
}

static void Arm(Session& session, std::uint32_t now = 10, std::uint32_t sequence = 2) {
    Frame request; request.opcode = static_cast<std::uint8_t>(Opcode::Arm); request.sequence = sequence;
    request.token = session.snapshot().token;
    const auto bytes = Encode(request);
    const Target pose{{1500, 1600, 1700}, 0};
    auto result = session.Handle(bytes.data(), bytes.size(), now, true, pose, 0);
    assert(result.accepted && result.action_required && !result.has_reply);
    std::array<std::uint8_t, 20> reply{};
    assert(session.CompleteAction(result.generation, sequence, true, now, reply));
}

static void VerifyCommandAndReadVectors() {
    for (const auto& vector : kCommands) {
        auto bytes = Command(vector.hex);
        Frame frame; DecodeError error;
        assert(Decode(bytes.data(), bytes.size(), frame, error));
        assert(Encode(frame) == bytes);
        if (frame.opcode == static_cast<std::uint8_t>(Opcode::SetTarget)) {
            Target target; Result result;
            assert(DecodeTarget(frame, target, result) && result == Result::Ok);
        } else {
            assert(PayloadIsZero(frame));
        }
    }

    for (const auto& vector : kEvents) {
        const auto bytes = Command(vector.hex);
        assert(bytes[0] == 1 && bytes[18] == 0 && bytes[19] == 0);
        if (bytes[1] == static_cast<std::uint8_t>(Opcode::AsyncState)) {
            Frame event; event.opcode = bytes[1]; event.sequence = U32(bytes.data() + 2); event.token = U32(bytes.data() + 6);
            std::copy(bytes.begin() + 10, bytes.end(), event.payload.begin());
            assert(Encode(event) == bytes);
            continue;
        }
        Frame request; request.opcode = bytes[1] & 0x7f; request.sequence = U32(bytes.data() + 2);
        Snapshot state; state.state = static_cast<ControlState>(bytes[11]);
        state.token = (bytes[16] & 8) ? U32(bytes.data() + 6) : 0;
        state.last_applied_sequence = U32(bytes.data() + 12);
        state.valid_mask = (bytes[16] & 1) ? 7 : 0;
        state.interpolating_mask = (bytes[16] & 2) ? 7 : 0;
        state.battery_percent = bytes[17];
        assert(EncodeReply(request, static_cast<Result>(bytes[10]), state, U32(bytes.data() + 6), (bytes[16] & 4) != 0) == bytes);
    }

    for (const auto& vector : kReads) {
        const auto bytes = Bytes(vector.hex);
        if (std::string(vector.name).find("device_info_") == 0) {
            assert(bytes.size() == 20);
            DeviceInfo info{}; info.firmware_major = bytes[2]; info.firmware_minor = bytes[3];
            info.firmware_patch = bytes[4]; info.hardware_profile = bytes[5]; info.capabilities = U32(bytes.data() + 6);
            info.protocol_minor = bytes[1]; info.security_policy = bytes[16];
            assert(EncodeDeviceInfo(info) == Fixed(bytes));
        } else if (std::string(vector.name).find("identity_") == 0) {
            assert(bytes.size() == 16);
            std::array<std::uint8_t, 16> identity{}; std::copy(bytes.begin(), bytes.end(), identity.begin());
            assert(EncodeIdentity(identity) == identity);
        } else {
            assert(bytes.size() == 20);
            Snapshot state; state.state = static_cast<ControlState>(bytes[1]); state.token = U32(bytes.data() + 2);
            state.last_applied_sequence = U32(bytes.data() + 6);
            state.channels = {U16(bytes.data() + 10), U16(bytes.data() + 12), U16(bytes.data() + 14)};
            state.valid_mask = bytes[16]; state.interpolating_mask = bytes[17]; state.battery_percent = bytes[18];
            assert(EncodeSnapshot(state) == Fixed(bytes));
        }
    }
}

static void VerifyManagementVectors() {
    for (const auto& vector : kManagementCommands) {
        const auto bytes = Command(vector.hex);
        Frame frame; DecodeError error;
        assert(Decode(bytes.data(), bytes.size(), frame, error));
        assert(Encode(frame) == bytes);
    }

    for (const auto& vector : kManagementEvents) {
        const auto bytes = Command(vector.hex);
        Frame request; request.opcode = bytes[1] & 0x7f;
        request.sequence = U32(bytes.data() + 2);
        request.token = U32(bytes.data() + 6);
        Snapshot state; state.state = static_cast<ControlState>(bytes[11]);
        state.token = (bytes[16] & 8) ? request.token : 0;
        state.last_applied_sequence = U32(bytes.data() + 12);
        state.valid_mask = (bytes[16] & 1) ? 7 : 0;
        state.interpolating_mask = (bytes[16] & 2) ? 7 : 0;
        state.battery_percent = bytes[17];
        assert(EncodeReply(request, static_cast<Result>(bytes[10]), state, request.token,
                           (bytes[16] & 4) != 0) == bytes);
    }

    // v1.1 stays a historical codec corpus. Runtime semantics for removed
    // transfer commands are asserted against the current v1.2 corpus below.
    assert(std::size(kManagementScenarios) == 17);
}

static void VerifySharedPairingVectors() {
    assert(std::string(kBuiltInProfileId) == "satori_c3_v1");
    assert(kSharedCommandTimeoutMs == 500 && kSharedCommandMaxRetries == 3);
    assert(kSharedReleaseAckWindowMs == static_cast<int>(kReleaseAckWindowMs));
    assert(std::string(kSharedReleaseAckWindowOrigin) == "action_completion");
    const auto profile = BuiltInStartupTarget();
    for (int i = 0; i < 3; ++i) {
        assert(profile.channels[i] == kBuiltInStartup[i]);
        assert(kBuiltInMinimum[i] == 500 && kBuiltInMaximum[i] == 2500);
    }
    for (const auto& vector : kSharedReads) {
        const auto bytes = Bytes(vector.hex);
        assert(bytes.size() == 20);
        DeviceInfo info{}; info.protocol_minor = bytes[1]; info.firmware_major = bytes[2];
        info.firmware_minor = bytes[3]; info.firmware_patch = bytes[4]; info.hardware_profile = bytes[5];
        info.capabilities = U32(bytes.data() + 6); info.security_policy = bytes[16];
        assert(EncodeDeviceInfo(info) == Fixed(bytes));
        assert(info.protocol_minor == 2 && info.capabilities == 0x1df && info.security_policy == 2);
    }
    for (const auto& vector : kSharedRejections) {
        const auto bytes = Command(vector.hex);
        Session session;
        session.Connect(SecurePeer(), true, 0);
        const auto claim = Command("0101010000000000000000000000000000000000");
        assert(session.Handle(claim.data(), claim.size(), 0, false, {}, 0x12345678).accepted);
        const auto result = session.Handle(bytes.data(), bytes.size(), 10, false, {}, 0);
        assert(result.result == Expected(vector.expected));
    }
    assert(std::size(kSharedScenarios) == 19);
    const auto has_shared_scenario = [](const char* name) {
        return std::any_of(std::begin(kSharedScenarios), std::end(kSharedScenarios),
                           [name](const char* item) { return std::string(item) == name; });
    };
    assert(has_shared_scenario("release_lost_ack_replayed_with_default_retry_timing"));
    assert(has_shared_scenario("release_window_starts_at_completion_and_does_not_extend"));
}

static Result Expected(const char* text) {
    const std::string value(text);
    if (value == "BAD_VERSION") return Result::BadVersion;
    if (value == "BAD_OPCODE") return Result::BadOpcode;
    if (value == "BAD_PAYLOAD") return Result::BadPayload;
    if (value == "OLD_SEQUENCE") return Result::OldSequence;
    if (value == "BAD_SESSION") return Result::BadSession;
    if (value == "NOT_ARMED") return Result::NotArmed;
    if (value == "NOT_AUTHORIZED") return Result::NotAuthorized;
    if (value == "NOT_CONFIGURED") return Result::NotConfigured;
    if (value == "SUBSCRIPTION_REQUIRED") return Result::SubscriptionRequired;
    return Result::BadLength;
}

static void VerifyRejectionVectors() {
    for (const auto& vector : kRejections) {
        const std::string name(vector.name);
        const auto bytes = Bytes(vector.hex);
        Session session;
        const bool unsubscribed = name == "unsubscribed_claim";
        Peer peer = SecurePeer();
        if (name == "unauthenticated_control") peer.authenticated = false;
        session.Connect(peer, !unsubscribed, 0);
        if (!unsubscribed && name != "unauthenticated_control") {
            const auto claim = Command("0101010000000000000000000000000000000000");
            auto outcome = session.Handle(claim.data(), claim.size(), 0, false, {}, 0x12345678);
            assert(outcome.result == Result::Ok);
            if (name != "set_not_armed") Arm(session, 10);
        }
        const auto before = session.snapshot();
        const auto outcome = session.Handle(bytes.data(), bytes.size(), 20, true, {}, 0);
        if (name == "unauthenticated_control") assert(outcome.result == Result::NotAuthorized);
        else if (name == "length_19" || name == "length_21") assert(outcome.result == Result::BadLength && !outcome.has_reply);
        else assert(outcome.result == Expected(vector.expected));
        if (name != "unsubscribed_claim" && name != "unauthenticated_control" && name != "length_19" && name != "length_21") {
            assert(session.snapshot().channels == before.channels && session.snapshot().valid_mask == before.valid_mask);
        }
        if (name == "unsubscribed_claim") {
            session.SetSubscribed(true);
            const auto claim = Command("0101010000000000000000000000000000000000");
            assert(session.Handle(claim.data(), claim.size(), 21, false, {}, 0xabcdef01).result == Result::Ok);
        } else if (name == "missing_verified_startup_error") {
            auto arm = Command("0107020000007856341200000000000000000000");
            assert(session.Handle(arm.data(), arm.size(), 21, false, {}, 0).result == Result::NotConfigured);
            const Target configured{{1500, 1600, 1700}, 0};
            const auto retried = session.Handle(arm.data(), arm.size(), 21, true, configured, 0);
            assert(retried.accepted && retried.action_required);
        } else if (name != "unauthenticated_control" && name != "sequence_zero") {
            Frame good; good.opcode = static_cast<std::uint8_t>(Opcode::GetStatus); good.sequence = 3;
            good.token = session.snapshot().token;
            const auto retry = Encode(good);
            assert(session.Handle(retry.data(), retry.size(), 21, true, {}, 0).accepted);
        }
    }
}

static void VerifyStateScenarios() {
    assert(std::size(kScenarios) == 12);
    const auto has_scenario = [](const char* name) {
        return std::any_of(std::begin(kScenarios), std::end(kScenarios), [name](const char* value) { return std::string(value) == name; });
    };
    assert(has_scenario("duplicate_target_idempotent"));
    assert(has_scenario("same_sequence_different_content"));
    assert(has_scenario("lost_claim_ack"));
    assert(has_scenario("late_target_after_halt"));
    assert(has_scenario("lease_expiration"));
    assert(has_scenario("invalid_traffic_not_keepalive"));
    assert(has_scenario("release_cannot_resurrect_claim"));
    assert(has_scenario("missing_startup_configuration"));
    assert(has_scenario("cold_claim_does_not_move"));
    assert(has_scenario("app_ui_recreation"));
    assert(has_scenario("seq_u32_not_u16"));
    assert(has_scenario("old_frame_after_cache_eviction"));

    // Lost CLAIM response replays the same allocated token.
    Session claim_session; Claim(claim_session);
    const auto claim = Command("0101010000000000000000000000000000000000");
    const auto claim_again = claim_session.Handle(claim.data(), claim.size(), 100, false, {}, 0xaabbccdd);
    assert(claim_again.duplicate && claim_again.reply[6] == 0x78);

    // ARM completion survives independent sequence traffic; retries get cached ACK.
    Frame arm; arm.opcode = static_cast<std::uint8_t>(Opcode::Arm); arm.sequence = 2; arm.token = 0x12345678;
    const auto arm_bytes = Encode(arm);
    const Target startup{{1500, 1600, 1700}, 0};
    auto arm_out = claim_session.Handle(arm_bytes.data(), 20, 10, true, startup, 0);
    Frame keep; keep.opcode = static_cast<std::uint8_t>(Opcode::Keepalive); keep.sequence = 3; keep.token = arm.token;
    const auto keep_bytes = Encode(keep);
    assert(claim_session.Handle(keep_bytes.data(), 20, 11, true, {}, 0).accepted);
    std::array<std::uint8_t, 20> reply{};
    assert(claim_session.CompleteAction(arm_out.generation, 2, true, 12, reply));
    assert(claim_session.Handle(arm_bytes.data(), 20, 12, true, {}, 0).has_reply);

    // Latest target may replace an unprocessed target; HALT is an immediate generation barrier.
    Frame target; target.opcode = static_cast<std::uint8_t>(Opcode::SetTarget); target.sequence = 4; target.token = arm.token;
    P16(target.payload.data(), 1500); P16(target.payload.data() + 2, 1600); P16(target.payload.data() + 4, 1700);
    const auto target1 = Encode(target); const auto old_target = claim_session.Handle(target1.data(), 20, 20, true, {}, 0);
    target.sequence = 5; P16(target.payload.data(), 1800); const auto target2 = Encode(target);
    const auto latest = claim_session.Handle(target2.data(), 20, 21, true, {}, 0);
    assert(old_target.accepted && latest.accepted && latest.reply[2] == 5);
    auto target1_replay = claim_session.Handle(target1.data(), 20, 22, true, {}, 0);
    assert(target1_replay.duplicate && target1_replay.has_reply && target1_replay.reply == old_target.reply);
    Frame conflict_target = target; conflict_target.sequence = 5; conflict_target.payload[0] ^= 1;
    const auto conflict_bytes = Encode(conflict_target);
    assert(claim_session.Handle(conflict_bytes.data(), 20, 22, true, {}, 0).result == Result::SequenceConflict);
    Frame halt; halt.opcode = static_cast<std::uint8_t>(Opcode::Halt); halt.sequence = 6; halt.token = arm.token;
    const auto halt_bytes = Encode(halt); const auto stopped = claim_session.Handle(halt_bytes.data(), 20, 22, true, {}, 0);
    assert(stopped.accepted && stopped.generation != latest.generation);
    assert(!claim_session.CompleteAction(latest.generation, 5, true, 23, reply));
    assert(claim_session.CompleteAction(stopped.generation, 6, true, 23, reply));
    const auto halt_retry = claim_session.Handle(halt_bytes.data(), 20, 23, true, {}, 0);
    assert(halt_retry.duplicate && halt_retry.has_reply && halt_retry.reply == reply);

    // Sequence boundary and cache eviction remain monotonic at u32 width.
    Session seq; Claim(seq); Arm(seq);
    auto setAt = [&](std::uint32_t number) {
        Frame f; f.opcode = static_cast<std::uint8_t>(Opcode::SetTarget); f.sequence = number; f.token = seq.snapshot().token;
        P16(f.payload.data(), 1500); P16(f.payload.data() + 2, 1600); P16(f.payload.data() + 4, 1700);
        return Encode(f);
    };
    auto large = setAt(65535); assert(seq.Handle(large.data(), 20, 30, true, {}, 0).accepted);
    large = setAt(65536); assert(seq.Handle(large.data(), 20, 31, true, {}, 0).accepted);
    large = setAt(65537); assert(seq.Handle(large.data(), 20, 32, true, {}, 0).accepted);
    auto sequence_replay = setAt(65535); assert(seq.Handle(sequence_replay.data(), 20, 33, true, {}, 0).duplicate);

    Session eviction; Claim(eviction); Arm(eviction);
    auto original = setAt(3); // use a distinct session token in the actual frame below
    Frame first; first.opcode = static_cast<std::uint8_t>(Opcode::SetTarget); first.sequence = 3; first.token = eviction.snapshot().token;
    P16(first.payload.data(), 1500); P16(first.payload.data() + 2, 1600); P16(first.payload.data() + 4, 1700);
    original = Encode(first);
    const auto first_target = eviction.Handle(original.data(), 20, 40, true, {}, 0);
    assert(first_target.accepted);
    assert(eviction.CompleteAction(first_target.generation, 3, true, 40, reply));
    for (std::uint32_t i = 4; i <= 20; ++i) {
        Frame f; f.opcode = static_cast<std::uint8_t>(Opcode::Keepalive); f.sequence = i; f.token = eviction.snapshot().token;
        const auto bytes = Encode(f); assert(eviction.Handle(bytes.data(), 20, 40 + i, true, {}, 0).accepted);
    }
    assert(eviction.Handle(original.data(), 20, 80, true, {}, 0).result == Result::OldSequence);

    // Missing startup data rejects ARM without consuming its sequence or enabling outputs.
    Session no_pose; Claim(no_pose);
    Frame first_arm; first_arm.opcode = static_cast<std::uint8_t>(Opcode::Arm); first_arm.sequence = 2; first_arm.token = no_pose.snapshot().token;
    const auto first_arm_bytes = Encode(first_arm);
    assert(no_pose.Handle(first_arm_bytes.data(), 20, 10, false, {}, 0).result == Result::NotConfigured);
    assert(no_pose.snapshot().valid_mask == 0);
    const Target verified_pose{{1500, 1600, 1700}, 0};
    const auto verified_arm = no_pose.Handle(first_arm_bytes.data(), 20, 11, true, verified_pose, 0);
    assert(verified_arm.accepted && no_pose.CompleteAction(verified_arm.generation, 2, true, 11, reply));
    assert(no_pose.snapshot().valid_mask == 7);

    // Invalid traffic cannot extend the lease.
    Session invalid_traffic; Claim(invalid_traffic); Arm(invalid_traffic, 10);
    Frame malformed; malformed.opcode = static_cast<std::uint8_t>(Opcode::SetTarget); malformed.sequence = 3;
    malformed.token = invalid_traffic.snapshot().token; malformed.payload[0] = 1;
    const auto malformed_bytes = Encode(malformed);
    for (std::uint32_t t : {1000u, 3000u, 6009u})
        assert(invalid_traffic.Handle(malformed_bytes.data(), 20, t, true, {}, 0).result == Result::BadPayload);
    assert(!invalid_traffic.TickLease(6009));
    assert(invalid_traffic.TickLease(6010) && !invalid_traffic.connected());

    // Lease expiry cancels the lease and makes the current link unusable; reconnect is fresh.
    Session lease; Claim(lease); Arm(lease, 10);
    assert(lease.TickLease(6010) && !lease.connected() && lease.snapshot().valid_mask == 7);
    const auto denied = lease.Handle(claim.data(), 20, 6011, false, {}, 0x87654321);
    assert(denied.result == Result::NotAuthorized);
    lease.Connect(SecurePeer(), true, 7000);
    assert(lease.Handle(claim.data(), 20, 7000, false, {}, 0x87654321).result == Result::Ok);
    assert(lease.snapshot().valid_mask == 7);
    const auto retained = lease.snapshot().channels;
    Frame rearm; rearm.opcode = static_cast<std::uint8_t>(Opcode::Arm); rearm.sequence = 2; rearm.token = lease.snapshot().token;
    const auto rearm_bytes = Encode(rearm);
    const auto rearmed = lease.Handle(rearm_bytes.data(), 20, 7001, false, {}, 0);
    assert(rearmed.accepted && !rearmed.action_required && rearmed.has_reply);
    assert(lease.snapshot().channels == retained && lease.snapshot().last_applied_sequence == 2);

    Session disconnected; Claim(disconnected); Arm(disconnected);
    const auto held_output = disconnected.snapshot().channels;
    disconnected.Disconnect();
    assert(!disconnected.connected() && disconnected.snapshot().token == 0);
    assert(disconnected.snapshot().valid_mask == 7 && disconnected.snapshot().channels == held_output);
    disconnected.Connect(SecurePeer(), true, 8000);
    assert(disconnected.snapshot().valid_mask == 7 && disconnected.snapshot().channels == held_output);

    // RELEASE ACK grace starts after action completion, is fixed, and only
    // permits exact replay of that RELEASE request.
    Session released; Claim(released); Arm(released);
    Frame release; release.opcode = static_cast<std::uint8_t>(Opcode::Release); release.sequence = 3; release.token = released.snapshot().token;
    const auto release_bytes = Encode(release);
    const auto release_out = released.Handle(release_bytes.data(), 20, 30, true, {}, 0);
    assert(release_out.accepted && release_out.action_required);
    assert(released.CompleteAction(release_out.generation, 3, true, 10000, reply));
    for (const std::uint32_t elapsed : {500u, 1500u, 2999u}) {
        auto replay = released.Handle(release_bytes.data(), 20, 10000 + elapsed, true, {}, 0);
        assert(replay.duplicate && replay.has_reply && replay.reply == reply);
    }
    const auto release_expired = 10000 + kReleaseAckWindowMs;
    assert(released.Handle(release_bytes.data(), 20, release_expired, true, {}, 0).result == Result::BadSession);
    assert(released.TickLease(release_expired));
    assert(!released.connected() && released.snapshot().token == 0 && released.snapshot().valid_mask == 7);
    assert(released.Handle(claim.data(), 20, 13001, false, {}, 0x99999999).result == Result::NotAuthorized);

    // Released sessions do not replay other cached replies or accept new
    // operations. Duplicate RELEASE retries never move the deadline.
    Session replay_only; Claim(replay_only); Arm(replay_only);
    Frame status_before; status_before.opcode = static_cast<std::uint8_t>(Opcode::GetStatus);
    status_before.sequence = 3; status_before.token = replay_only.snapshot().token;
    const auto status_before_bytes = Encode(status_before);
    assert(replay_only.Handle(status_before_bytes.data(), 20, 20, false, {}, 0).accepted);
    Frame release_only; release_only.opcode = static_cast<std::uint8_t>(Opcode::Release);
    release_only.sequence = 4; release_only.token = replay_only.snapshot().token;
    const auto release_only_bytes = Encode(release_only);
    const auto release_only_out = replay_only.Handle(release_only_bytes.data(), 20, 21, false, {}, 0);
    assert(replay_only.CompleteAction(release_only_out.generation, 4, true, 25, reply));
    assert(replay_only.Handle(status_before_bytes.data(), 20, 26, false, {}, 0).result == Result::BadSession);
    assert(replay_only.Handle(status_before_bytes.data(), 20, 27, false, {}, 0).result == Result::BadSession);
    Frame stale_target; stale_target.opcode = static_cast<std::uint8_t>(Opcode::SetTarget);
    stale_target.sequence = 5; stale_target.token = release_only.token;
    P16(stale_target.payload.data(), 500); P16(stale_target.payload.data() + 2, 500); P16(stale_target.payload.data() + 4, 500);
    const auto stale_target_bytes = Encode(stale_target);
    const auto held_after_release = replay_only.snapshot().channels;
    assert(replay_only.Handle(stale_target_bytes.data(), 20, 27, true, {}, 0).result == Result::BadSession);
    assert(replay_only.snapshot().channels == held_after_release && replay_only.snapshot().token == 0);
    assert(replay_only.Handle(release_only_bytes.data(), 20, 28, false, {}, 0).duplicate);
    assert(replay_only.Handle(release_only_bytes.data(), 20, 25 + kReleaseAckWindowMs - 1, false, {}, 0).duplicate);
    const auto replay_deadline = 25 + kReleaseAckWindowMs;
    assert(replay_only.Handle(release_only_bytes.data(), 20, replay_deadline, false, {}, 0).result == Result::BadSession);
    assert(replay_only.TickLease(replay_deadline));
    assert(!replay_only.connected() && !replay_only.snapshot().token);

    // Deadline arithmetic remains correct across uint32 wrap, and periodic
    // lease polling also closes exactly at the fixed deadline.
    Session release_wrap; Claim(release_wrap); Arm(release_wrap);
    Frame wrap_release; wrap_release.opcode = static_cast<std::uint8_t>(Opcode::Release);
    wrap_release.sequence = 3; wrap_release.token = release_wrap.snapshot().token;
    const auto wrap_bytes = Encode(wrap_release);
    const auto wrap_out = release_wrap.Handle(wrap_bytes.data(), 20, 50, false, {}, 0);
    const std::uint32_t completion_wrap = 0xfffffff0u;
    assert(release_wrap.CompleteAction(wrap_out.generation, 3, true, completion_wrap, reply));
    const auto before_wrap_deadline = static_cast<std::uint32_t>(completion_wrap + kReleaseAckWindowMs - 1);
    assert(release_wrap.Handle(wrap_bytes.data(), 20, before_wrap_deadline, false, {}, 0).duplicate);
    const auto at_wrap_deadline = static_cast<std::uint32_t>(completion_wrap + kReleaseAckWindowMs);
    assert(release_wrap.TickLease(at_wrap_deadline) && !release_wrap.connected());

    // Cold CLAIM never initializes outputs; repeated runtime snapshots are read-only.
    Session cold; Claim(cold);
    assert(cold.snapshot().valid_mask == 0 && cold.snapshot().token != 0);
    const auto token = cold.snapshot().token;
    Frame status; status.opcode = static_cast<std::uint8_t>(Opcode::GetStatus); status.sequence = 2; status.token = token;
    const auto status_bytes = Encode(status);
    assert(cold.Handle(status_bytes.data(), 20, 1, false, {}, 0).accepted);
    assert(cold.snapshot().token == token && cold.snapshot().valid_mask == 0);
    const auto snapshot_after_ui_recreate = cold.snapshot();
    assert(cold.snapshot().token == snapshot_after_ui_recreate.token && cold.snapshot().valid_mask == 0);

    // Legacy map is kept exactly: 1500/1600/1700 -> 90/99/100.8 degrees.
    const auto mapped = LogicalTargetToAngles(Target{{1500, 1600, 1700}, 0});
    assert(mapped.value[0] == 90.0f && mapped.value[1] == 99.0f);
    assert(mapped.value[2] > 100.79f && mapped.value[2] < 100.81f);
    MotionEngine motion; motion.Initialize({1500, 1600, 1700});
    motion.SetTarget(Target{{2000, 1600, 1500}, 200}, 1000, 7);
    assert(!motion.Tick(1010, 7) && motion.Tick(1020, 7));
    motion.Halt(8); const auto held = motion.issued();
    motion.SetTarget(Target{{500, 2500, 500}, 200}, 1030, 7); // stale pre-HALT callback
    assert(!motion.Tick(2000, 8) && motion.issued() == held);

    // A CH3 blink retarget does not restart unchanged CH1/CH2 trajectories.
    MotionEngine independent; independent.Initialize({1500, 1500, 1500});
    independent.SetTarget(Target{{2000, 1000, 1500}, 1000}, 1000, 9);
    assert(independent.Tick(1400, 9));
    const auto before_blink = independent.issued();
    assert(before_blink[0] > 1500 && before_blink[0] < 2000);
    independent.SetTarget(Target{{2000, 1000, 2000}, 100}, 1400, 9);
    assert(independent.interpolating_mask() == 7);
    assert(independent.Tick(1500, 9));
    const auto during_blink = independent.issued();
    assert(during_blink[0] == 1750 && during_blink[1] == 1250 && during_blink[2] == 2000);
    assert(independent.interpolating_mask() == 3);
    assert(independent.Tick(2000, 9));
    assert((independent.issued() == std::array<std::uint16_t, 3>{2000, 1000, 2000}));
    assert(independent.interpolating_mask() == 0);

    // A zero-duration change affects only changed channels; unchanged motion continues.
    independent.Initialize({1500, 1500, 1500});
    independent.SetTarget(Target{{2000, 1000, 1500}, 1000}, 3000, 10);
    assert(independent.Tick(3400, 10));
    independent.SetTarget(Target{{2000, 1000, 1900}, 0}, 3400, 10);
    assert(independent.issued()[2] == 1900 && independent.interpolating_mask() == 3);
    assert(independent.Tick(3500, 10));
    assert(independent.issued()[0] == 1750 && independent.issued()[1] == 1250);

    // Invalid scalar targets are ignored before interpolation state is changed.
    const auto safe = independent.issued();
    independent.SetTarget(Target{{2501, 1000, 1900}, 200}, 3510, 10);
    assert(independent.issued() == safe && independent.interpolating_mask() == 3);
}

int main() {
    Diagnostics diagnostics{3, StopReason::LeaseExpired, PwmFault, 0x12345678, 9, 2, 5, 0x213};
    const auto encoded = EncodeDiagnostics(diagnostics);
    assert(encoded == Command("0103040178563412090000000200050013020000"));
    // Diagnostic observation cannot renew a session lease or initialize motion.
    Session observed; Claim(observed);
    (void)EncodeDiagnostics(diagnostics);
    assert(observed.TickLease(6000));
    assert(observed.snapshot().valid_mask == 0);
    std::uint16_t saturated = UINT16_MAX;
    IncrementDiagnosticCounter(saturated);
    assert(saturated == UINT16_MAX);
    VerifyCommandAndReadVectors();
    VerifyManagementVectors();
    VerifySharedPairingVectors();
    VerifyRejectionVectors();
    VerifyStateScenarios();
    std::cout << "BLE v1 vectors and session/motion scenarios passed\n";
}
