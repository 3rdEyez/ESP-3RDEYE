#include "ble_session.hpp"

namespace satori::ble {
namespace {
bool IsZeroPayload(const Frame& f) { return PayloadIsZero(f); }
std::uint32_t Read32(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
}

void Session::Connect(const Peer& peer, bool event_subscribed, std::uint32_t now_ms) {
    Disconnect(); peer_ = peer; connected_ = true; subscribed_ = event_subscribed; last_valid_ms_ = now_ms;
}
void Session::SetSubscribed(bool subscribed) {
    if (connected_) subscribed_ = subscribed;
}
void Session::Disconnect() {
    ++generation_; if (generation_ == 0) ++generation_;
    connected_ = false; subscribed_ = false; has_owner_ = false; released_ = false;
    highest_sequence_ = 0; release_deadline_ms_ = 0; last_valid_ms_ = 0;
    const auto issued = snapshot_.channels;
    const auto valid = snapshot_.valid_mask;
    const auto battery = snapshot_.battery_percent;
    const auto applied = snapshot_.last_applied_sequence;
    cache_ = {}; cache_cursor_ = 0; snapshot_ = {};
    snapshot_.channels = issued; snapshot_.valid_mask = valid; snapshot_.battery_percent = battery;
    snapshot_.last_applied_sequence = applied;
    pending_action_ = {}; pending_action_valid_ = false;
    pending_opcode_ = Opcode::GetStatus; pending_target_ = {};
}
bool Session::Authorized() const { return connected_ && peer_.encrypted && peer_.authenticated && peer_.bonded; }
void Session::ClearSession(bool clear_cache) {
    ++generation_; if (generation_ == 0) ++generation_;
    snapshot_.token = 0; snapshot_.state = ControlState::Unclaimed;
    snapshot_.interpolating_mask = 0; has_owner_ = false;
    if (clear_cache) { cache_ = {}; cache_cursor_ = 0; highest_sequence_ = 0; }
}
Outcome Session::Reject(const Frame& request, Result result) const {
    Outcome out; out.request = request; out.result = result;
    out.reply = EncodeReply(request, result, snapshot_, request.token, peer_.bonded);
    return out;
}
void Session::Cache(const Frame& request, const std::array<std::uint8_t, kFrameSize>& reply) {
    auto& entry = cache_[cache_cursor_++ % cache_.size()]; entry.used = true; entry.sequence = request.sequence;
    entry.request = Encode(request); entry.reply = reply;
}

Outcome Session::Handle(const std::uint8_t* bytes, std::size_t length, std::uint32_t now_ms,
                        bool startup_configured, const Target& startup_target, std::uint32_t claim_token) {
    Frame req; DecodeError decode_error;
    if (length != kFrameSize || !bytes) { Outcome out; out.has_reply = false; out.result = Result::BadLength; return out; }
    if (bytes[0] != kProtocolVersion) {
        Frame malformed; malformed.version = bytes[0]; malformed.opcode = bytes[1];
        malformed.sequence = Read32(bytes + 2); malformed.token = Read32(bytes + 6);
        return Reject(malformed, Result::BadVersion);
    }
    if (bytes[1] < 1 || bytes[1] > 8) {
        Frame malformed; malformed.version = bytes[0]; malformed.opcode = bytes[1]; malformed.sequence = Read32(bytes + 2); malformed.token = Read32(bytes + 6);
        return Reject(malformed, Result::BadOpcode);
    }
    if (!Decode(bytes, length, req, decode_error)) { Outcome out; out.has_reply = false; out.result = Result::BadLength; return out; }
    Outcome out; out.request = req;
    if (!Authorized()) return Reject(req, Result::NotAuthorized);
    if (req.sequence == 0) return Reject(req, Result::OldSequence);

    const auto request_bytes = Encode(req);
    for (const auto& entry : cache_) if (entry.used && entry.sequence == req.sequence) {
        if (entry.request == request_bytes) { out.reply = entry.reply; out.duplicate = true; out.accepted = true; out.result = Result::Ok; return out; }
        return Reject(req, Result::SequenceConflict);
    }
    if (pending_action_valid_ && pending_action_.sequence == req.sequence && req.sequence != 0) {
        if (Encode(pending_action_) == request_bytes) { out.accepted = true; out.duplicate = true; out.has_reply = false; out.generation = generation_; return out; }
        return Reject(req, Result::SequenceConflict);
    }
    if (released_) return Reject(req, Result::BadSession);
    if (req.sequence <= highest_sequence_) return Reject(req, Result::OldSequence);

    const auto opcode = static_cast<Opcode>(req.opcode);
    if (opcode == Opcode::Claim) {
        if (released_ || has_owner_) return Reject(req, Result::BadSession);
        if (!subscribed_) return Reject(req, Result::SubscriptionRequired);
        if (req.token != 0 || !IsZeroPayload(req) || req.sequence != 1) return Reject(req, Result::BadPayload);
        if (claim_token == 0) return Reject(req, Result::InternalError);
        snapshot_.token = claim_token; snapshot_.state = ControlState::Holding; has_owner_ = true;
        highest_sequence_ = req.sequence; last_valid_ms_ = now_ms; out.accepted = true;
        out.reply = EncodeReply(req, Result::Ok, snapshot_, claim_token, true); Cache(req, out.reply); return out;
    }
    if (!has_owner_ || released_ || req.token == 0 || req.token != snapshot_.token)
        return Reject(req, Result::BadSession);

    if (pending_action_valid_) {
        const bool can_supersede_target = opcode == Opcode::SetTarget && pending_opcode_ == Opcode::SetTarget;
        const bool is_stop_barrier = opcode == Opcode::Halt || opcode == Opcode::Release;
        const bool is_management = false;
        const bool independent = opcode == Opcode::Keepalive || opcode == Opcode::GetStatus;
        if (!can_supersede_target && !is_stop_barrier && !is_management && !independent) return Reject(req, Result::Busy);
    }
    out.result = Result::Ok;
    switch (opcode) {
    case Opcode::Arm:
        if (!IsZeroPayload(req)) return Reject(req, Result::BadPayload);
        // If outputs were already initialized before this connection, ARM is an
        // idempotent authorization and must not apply the cold-start pose again.
        out.action_required = snapshot_.valid_mask == 0;
        if (out.action_required) {
            if (!startup_configured) return Reject(req, Result::NotConfigured);
            if (startup_target.transition_ms > kMaxTransitionMs) return Reject(req, Result::NotConfigured);
            for (const auto channel : startup_target.channels) if (channel < 500 || channel > 2500) return Reject(req, Result::NotConfigured);
            out.target = startup_target;
        }
        break;
    case Opcode::SetTarget:
        if (!DecodeTarget(req, out.target, out.result)) return Reject(req, out.result);
        if (!snapshot_.valid_mask) return Reject(req, Result::NotArmed);
        out.action_required = true;
        break;
    case Opcode::SetPairingCode: {
        std::uint32_t code = 0;
        if (!DecodePairingCode(req, code, out.result)) return Reject(req, Result::BadPayload);
        out.action_required = true;
        break;
    }
    case Opcode::Halt:
    case Opcode::Release:
    case Opcode::Keepalive:
    case Opcode::GetStatus:
        if (!IsZeroPayload(req)) return Reject(req, Result::BadPayload);
        out.action_required = opcode == Opcode::Halt || opcode == Opcode::Release;
        break;
    default: return Reject(req, Result::BadOpcode);
    }

    // GET_STATUS is a read and must not renew the lease. Everything else was validated above.
    if (opcode != Opcode::GetStatus) last_valid_ms_ = now_ms;
    highest_sequence_ = req.sequence;
    if (opcode == Opcode::Arm && !out.action_required) snapshot_.last_applied_sequence = req.sequence;
    if (opcode == Opcode::Halt || opcode == Opcode::Release) { ++generation_; if (generation_ == 0) ++generation_; }
    out.accepted = true; out.generation = generation_;
    if (opcode == Opcode::SetTarget) {
        out.reply = EncodeReply(req, Result::Ok, snapshot_, snapshot_.token, true); Cache(req, out.reply);
        pending_action_ = req; pending_action_valid_ = true;
        pending_opcode_ = Opcode::SetTarget; pending_target_ = out.target;
        return out;
    }
    if (out.action_required) {
        // ACK is deliberately withheld until CompleteAction observes the state change.
        out.has_reply = false; pending_action_ = req; pending_action_valid_ = true;
        pending_opcode_ = opcode; pending_target_ = out.target;
        return out;
    }
    out.reply = EncodeReply(req, Result::Ok, snapshot_, snapshot_.token, true); Cache(req, out.reply);
    return out;
}

bool Session::CompleteAction(std::uint32_t generation, std::uint32_t sequence, bool success,
                             std::array<std::uint8_t, kFrameSize>& reply) {
    if (!connected_ || !has_owner_ || generation != generation_ || !pending_action_valid_ ||
        sequence == 0 || sequence != pending_action_.sequence) return false;
    if (!success) return false;
    const auto opcode = pending_opcode_;
    if (opcode == Opcode::Arm || opcode == Opcode::SetTarget || opcode == Opcode::Halt ||
        opcode == Opcode::Release)
        snapshot_.last_applied_sequence = sequence;
    if (opcode == Opcode::Arm) {
        if (!snapshot_.valid_mask) { snapshot_.channels = pending_target_.channels; snapshot_.valid_mask = 7; }
        snapshot_.interpolating_mask = 0; snapshot_.state = ControlState::Holding;
    } else if (opcode == Opcode::SetTarget) {
        snapshot_.last_applied_sequence = sequence;
        pending_action_ = {}; pending_action_valid_ = false;
        pending_opcode_ = Opcode::GetStatus; pending_target_ = {};
        return true;
    } else if (opcode == Opcode::Halt) {
        snapshot_.interpolating_mask = 0; snapshot_.state = ControlState::Holding;
    } else if (opcode == Opcode::SetPairingCode) {
        reply = EncodeReply(pending_action_, Result::Ok, snapshot_, snapshot_.token, true); Cache(pending_action_, reply);
        pending_action_ = {}; pending_action_valid_ = false;
        pending_opcode_ = Opcode::GetStatus; pending_target_ = {};
        return true;
    } else if (opcode == Opcode::Release) {
        const auto old_token = snapshot_.token;
        released_ = true; release_deadline_ms_ = last_valid_ms_ + 500;
        snapshot_.interpolating_mask = 0; has_owner_ = false; snapshot_.token = 0; snapshot_.state = ControlState::Unclaimed;
        cache_ = {}; cache_cursor_ = 0;
        reply = EncodeReply(pending_action_, Result::Ok, snapshot_, old_token, true); Cache(pending_action_, reply);
        pending_action_ = {}; pending_action_valid_ = false;
        pending_opcode_ = Opcode::GetStatus; pending_target_ = {};
        return true;
    }
    reply = EncodeReply(pending_action_, Result::Ok, snapshot_, snapshot_.token, true); Cache(pending_action_, reply);
    pending_action_ = {}; pending_action_valid_ = false;
    pending_opcode_ = Opcode::GetStatus; pending_target_ = {};
    return true;
}

bool Session::FailAction(std::uint32_t generation, std::uint32_t sequence, Result result,
                         std::array<std::uint8_t, kFrameSize>& reply) {
    if (!connected_ || !has_owner_ || generation != generation_ || !pending_action_valid_ ||
        sequence == 0 || sequence != pending_action_.sequence) return false;
    reply = EncodeReply(pending_action_, result, snapshot_, snapshot_.token, true);
    Cache(pending_action_, reply);
    pending_action_ = {}; pending_action_valid_ = false;
    pending_opcode_ = Opcode::GetStatus; pending_target_ = {};
    return true;
}
bool Session::TickLease(std::uint32_t now_ms) {
    if (has_owner_ && !released_ && static_cast<std::uint32_t>(now_ms - last_valid_ms_) >= kLeaseTimeoutMs) {
        snapshot_.interpolating_mask = 0; ClearSession(true); connected_ = false; subscribed_ = false;
        peer_ = {}; pending_action_ = {}; pending_action_valid_ = false;
        pending_opcode_ = Opcode::GetStatus; pending_target_ = {};
        return true;
    }
    if (released_ && static_cast<std::int32_t>(now_ms - release_deadline_ms_) >= 0) { Disconnect(); return true; }
    return false;
}
void Session::UpdateMotion(const std::array<std::uint16_t, 3>& issued, std::uint8_t interpolating_mask,
                           std::uint32_t applied_sequence) {
    snapshot_.channels = issued; snapshot_.interpolating_mask = interpolating_mask & snapshot_.valid_mask;
    snapshot_.last_applied_sequence = applied_sequence;
    snapshot_.state = has_owner_ ? (snapshot_.interpolating_mask ? ControlState::Interpolating : ControlState::Holding) : ControlState::Unclaimed;
}

} // namespace satori::ble
