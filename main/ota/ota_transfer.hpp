#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace satori::ota {
constexpr std::uint32_t kSlotSize = 0x170000;
enum class Slot { Ota0, Ota1, Other };
struct Manifest {
    std::string_view board;
    std::string_view chip;
    Slot target_slot{Slot::Other};
    std::uint32_t image_size{0};
    std::array<std::uint8_t, 32> sha256{};
};
struct StartContext {
    bool explicit_request{false};
    bool control_idle_or_stopped{false};
    Slot running_slot{Slot::Other};
    bool running_slot_valid{false};
};
enum class Verification { IntegrityFailure, AuthenticityFailure, Verified };
// SHA-256 detects corruption only. Verified requires BOTH exact-image integrity
// and an independent platform signature/authenticity policy. The default fails
// closed; an implementation must never map a matching digest alone to Verified.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool Begin(const Manifest&) = 0;
    virtual bool Write(const std::uint8_t*, std::size_t) = 0;
    // Must safely clean partial Begin or failed Finish; core calls at most once.
    virtual void Abort() = 0;
    virtual Verification FinishVerifyAuthenticity() {
        return Verification::AuthenticityFailure;
    }
    virtual bool SetBootTarget(Slot) = 0;
};
enum class State { Fresh, Receiving, Committed, Failed, Cancelled };
enum class Error {
    None, RequestRequired, ControlActive, RunningSlotInvalid,
    ManifestMismatch, TargetInvalid, ImageSizeInvalid, TimeoutInvalid,
    BeginFailed, WriteFailed, Overlong, Truncated, IntegrityFailed,
    AuthenticityFailed, BootSelectionFailed, Timeout, Cancelled, InvalidData,
};
// One transfer per instance; never resumes a cancelled/failed stream or an old
// control action. Caller retains a control-stop gate throughout this lifetime.
// Sink operations are synchronous. Platform must bound each operation/network
// wait separately; Tick can enforce the total deadline between operations only.
class Transfer {
public:
    explicit Transfer(Sink& sink) : sink_(sink) {}
    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;
    ~Transfer() { if (state_ == State::Receiving) Cancel(); }

    bool Start(const Manifest& manifest, const StartContext& context,
               std::uint32_t now_ms, std::uint32_t timeout_ms = 120000) {
        if (state_ != State::Fresh) return false;
        if (!context.explicit_request) return Reject(Error::RequestRequired);
        if (!context.control_idle_or_stopped) return Reject(Error::ControlActive);
        if (!IsOta(context.running_slot) || !context.running_slot_valid)
            return Reject(Error::RunningSlotInvalid);
        if (manifest.board != "satori_c3_v1" || manifest.chip != "esp32c3")
            return Reject(Error::ManifestMismatch);
        if (!IsOta(manifest.target_slot) || manifest.target_slot == context.running_slot)
            return Reject(Error::TargetInvalid);
        if (manifest.image_size == 0 || manifest.image_size > kSlotSize)
            return Reject(Error::ImageSizeInvalid);
        // Unsigned subtraction supports uptime wrap, with a bounded interval.
        if (timeout_ms == 0 || timeout_ms >= 0x80000000u)
            return Reject(Error::TimeoutInvalid);
        target_ = manifest.target_slot;
        expected_ = manifest.image_size;
        start_ms_ = now_ms;
        timeout_ms_ = timeout_ms;
        state_ = State::Receiving;
        owns_sink_ = true; // Begin can fail after allocating a partial handle.
        if (!sink_.Begin(manifest)) return Fail(Error::BeginFailed);
        return true;
    }
    bool Write(const std::uint8_t* bytes, std::size_t size, std::uint32_t now_ms) {
        if (!Tick(now_ms)) return false;
        if (size > expected_ - received_) return Fail(Error::Overlong);
        if (size == 0) return true;
        if (!bytes) return Fail(Error::InvalidData);
        if (!sink_.Write(bytes, size)) return Fail(Error::WriteFailed);
        received_ += static_cast<std::uint32_t>(size);
        return true;
    }
    bool Finalize(std::uint32_t now_ms) {
        if (!Tick(now_ms)) return false;
        if (received_ != expected_) return Fail(Error::Truncated);
        switch (sink_.FinishVerifyAuthenticity()) {
        case Verification::IntegrityFailure: return Fail(Error::IntegrityFailed);
        case Verification::AuthenticityFailure: return Fail(Error::AuthenticityFailed);
        case Verification::Verified: break;
        default: return Fail(Error::AuthenticityFailed);
        }
        if (!sink_.SetBootTarget(target_)) return Fail(Error::BootSelectionFailed);
        owns_sink_ = false;
        state_ = State::Committed;
        return true;
    }
    bool Tick(std::uint32_t now_ms) {
        if (state_ != State::Receiving) return false;
        if (static_cast<std::uint32_t>(now_ms - start_ms_) >= timeout_ms_)
            return Fail(Error::Timeout);
        return true;
    }
    void Cancel() {
        if (state_ == State::Committed || state_ == State::Failed || state_ == State::Cancelled) return;
        error_ = Error::Cancelled;
        state_ = State::Cancelled;
        AbortOnce();
    }
    State state() const { return state_; }
    Error error() const { return error_; }
    std::uint32_t received() const { return received_; }
private:
    static bool IsOta(Slot slot) { return slot == Slot::Ota0 || slot == Slot::Ota1; }
    bool Reject(Error error) { error_ = error; state_ = State::Failed; return false; }
    bool Fail(Error error) { Reject(error); AbortOnce(); return false; }
    void AbortOnce() {
        if (!owns_sink_) return;
        owns_sink_ = false;
        sink_.Abort();
    }
    Sink& sink_;
    State state_{State::Fresh};
    Error error_{Error::None};
    Slot target_{Slot::Other};
    std::uint32_t expected_{0}, received_{0}, start_ms_{0}, timeout_ms_{0};
    bool owns_sink_{false};
};
} // namespace satori::ota
