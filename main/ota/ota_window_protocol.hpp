#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace satori::ota {
// Optional characteristic 0007 schema; standard control protocol stays v1.2.
enum class WindowState : std::uint8_t {
    Closed = 0, Opening = 1, Open = 2, Uploading = 3, Closing = 4, Committed = 5, Failed = 6,
};
enum class WindowResult : std::uint8_t {
    Ok = 0, Busy = 1, Unsupported = 2, Invalid = 3, NotReady = 4, StaleWindow = 5, Internal = 6,
};
struct WindowCommand {
    std::uint8_t action{0}; // 1=open, 2=close
    std::uint32_t request_id{0}, window_id{0};
};
struct WindowStatus {
    WindowState state{WindowState::Closed};
    WindowResult result{WindowResult::Ok};
    std::uint32_t ack_request_id{0}, window_id{0}, remaining_ms{0};
};
inline bool ValidWindowCommand(const WindowCommand& command) {
    return command.request_id != 0 &&
           ((command.action == 1 && command.window_id == 0) ||
            (command.action == 2 && command.window_id != 0));
}
inline std::uint32_t WindowReadLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}
inline void WindowWriteLe32(std::uint8_t* bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8*i));
}
inline bool DecodeWindowCommand(const std::uint8_t* bytes, std::size_t size, WindowCommand& out) {
    if (!bytes || size != 10 || bytes[0] != 1) return false;
    const WindowCommand command{bytes[1], WindowReadLe32(bytes+2), WindowReadLe32(bytes+6)};
    if (!ValidWindowCommand(command)) return false;
    out = command;
    return true;
}
// Invalid commands encode as all-zero bytes, which cannot decode as schema 1.
inline std::array<std::uint8_t,10> EncodeWindowCommand(const WindowCommand& command) {
    std::array<std::uint8_t,10> bytes{};
    if (!ValidWindowCommand(command)) return bytes;
    bytes[0] = 1; bytes[1] = command.action;
    WindowWriteLe32(bytes.data()+2, command.request_id);
    WindowWriteLe32(bytes.data()+6, command.window_id);
    return bytes;
}
inline bool WindowPrintableAscii(std::string_view text) {
    for (unsigned char byte : text) if (byte < 0x20 || byte > 0x7e) return false;
    return true;
}
inline bool WindowEnumsValid(const WindowStatus& status) {
    return static_cast<unsigned>(status.state) <= 6 && static_cast<unsigned>(status.result) <= 6;
}
inline std::vector<std::uint8_t> EncodeWindowStatus(
        const WindowStatus& status, std::string_view ssid, std::string_view password) {
    if (!WindowEnumsValid(status) || ssid.size() > 32 || password.size() > 64 ||
        !WindowPrintableAscii(ssid) || !WindowPrintableAscii(password)) return {};
    std::vector<std::uint8_t> bytes(18+ssid.size()+password.size(), 0);
    bytes[0] = 1; bytes[1] = static_cast<std::uint8_t>(status.state);
    bytes[2] = static_cast<std::uint8_t>(status.result);
    WindowWriteLe32(bytes.data()+4, status.ack_request_id);
    WindowWriteLe32(bytes.data()+8, status.window_id);
    WindowWriteLe32(bytes.data()+12, status.remaining_ms);
    bytes[16] = static_cast<std::uint8_t>(ssid.size());
    bytes[17] = static_cast<std::uint8_t>(password.size());
    std::size_t at = 18;
    for (unsigned char byte : ssid) bytes[at++] = byte;
    for (unsigned char byte : password) bytes[at++] = byte;
    return bytes;
}
// Decoded views borrow the input buffer; caller must retain it while using them.
inline bool DecodeWindowStatus(const std::uint8_t* bytes, std::size_t size,
        WindowStatus& out, std::string_view& ssid, std::string_view& password) {
    if (!bytes || size < 18 || bytes[0] != 1 || bytes[3] != 0 || bytes[16] > 32 || bytes[17] > 64 ||
        size != static_cast<std::size_t>(18+bytes[16]+bytes[17])) return false;
    const WindowStatus status{static_cast<WindowState>(bytes[1]), static_cast<WindowResult>(bytes[2]),
                             WindowReadLe32(bytes+4), WindowReadLe32(bytes+8), WindowReadLe32(bytes+12)};
    if (!WindowEnumsValid(status)) return false;
    const std::string_view name(reinterpret_cast<const char*>(bytes+18), bytes[16]);
    const std::string_view secret(reinterpret_cast<const char*>(bytes+18+bytes[16]), bytes[17]);
    if (!WindowPrintableAscii(name) || !WindowPrintableAscii(secret)) return false;
    out = status; ssid = name; password = secret;
    return true;
}
inline WindowResult AdmitWindowCommand(const WindowCommand& command, WindowState state,
        std::uint32_t current_window, bool native_ready) {
    if (!ValidWindowCommand(command) || static_cast<unsigned>(state) > 6) return WindowResult::Invalid;
    if (command.action == 1) {
        if (state != WindowState::Closed && state != WindowState::Failed) return WindowResult::Busy;
        return native_ready ? WindowResult::Ok : WindowResult::Unsupported;
    }
    if (command.window_id != current_window) return WindowResult::StaleWindow;
    if (state == WindowState::Committed) return WindowResult::Busy;
    return WindowResult::Ok;
}
struct WindowAdmission {
    WindowResult result{WindowResult::Invalid};
    bool execute{false}, duplicate{false};
};
// Bounded eight-entry retry cache. Owner serializes access and retains this object
// across asynchronous state transitions; no timers, locks, strings or actions.
class WindowProtocol {
public:
    WindowAdmission Admit(const WindowCommand& command, WindowState state,
                          std::uint32_t current_window, bool native_ready) {
        for (const auto& cached : cache_) {
            if (!cached.occupied || command.request_id != cached.command.request_id) continue;
            if (command.action != cached.command.action || command.window_id != cached.command.window_id)
                return {WindowResult::Invalid, false, false};
            return {cached.result, false, true};
        }
        const auto result = AdmitWindowCommand(command, state, current_window, native_ready);
        // Invalid wire input never replaces a valid idempotency entry. Retried
        // entries do not advance eviction; at most eight distinct requests stay.
        if (ValidWindowCommand(command)) {
            cache_[next_] = {command, result, true};
            next_ = (next_ + 1) % cache_.size();
        }
        const bool execute = result == WindowResult::Ok &&
            (command.action == 1 || (state != WindowState::Closed && state != WindowState::Closing));
        return {result, execute, false};
    }
private:
    struct CachedRequest {
        WindowCommand command{};
        WindowResult result{WindowResult::Invalid};
        bool occupied{false};
    };
    std::array<CachedRequest,8> cache_{};
    std::size_t next_{0};
};
} // namespace satori::ota
